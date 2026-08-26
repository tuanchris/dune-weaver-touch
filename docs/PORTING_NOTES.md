# Porting notes: QML touch app → ESP32-S3 LVGL

Distilled from the reference implementation at
`dune-weaver-pi/dune-weaver-touch` (PySide6/QML, `firmware_client.py`,
`backend.py`, `discovery.py`, `thr_preview.py`) and cross-checked against the
Pi backend's `modules/connection/fluidnc_client.py`. This is the contract for
increments 2+. Exact values here were lifted from working code — don't guess
replacements.

## 1. Firmware HTTP API

Base: `http://<host>[:port]`, port 80 default, plain HTTP, stateless,
multi-client. **Poll to confirm; never trust a command's return value.**

### Auth
- `$Sand/Password` set on board ⇒ every request needs `X-Sand-Key: <plaintext>`;
  missing/wrong key ⇒ 401. Store the password base64 in local settings.
  (ESP32 exception: NVS is binary, not a JSON file — this firmware stores the
  password plaintext in the `dwt` NVS namespace instead of base64.)
- 401 during a status poll ⇒ immediately "disconnected — needs password" (no
  fail-streak grace; 401 is deterministic).

### Read endpoints
| Route | Returns | Timeout |
|---|---|---|
| `GET /sand_status` | status JSON (below) | 12 s |
| `GET /sand_patterns` | array of paths relative to `/patterns` (`"a.thr"`, `"sub/x.thr"`); has `ETag`, honors `If-None-Match` → 304. Conditional GETs are exempt from low-heap shedding — always cache the ETag | 6 s |
| `GET /sand_playlists` | array of `.txt` filenames | 6 s |
| `GET /sand_settings` | flat dict `"Namespace/Key" -> string` | 6 s |
| `GET /sd/<path>` | raw file bytes (~30–60 KB/s!). Convention in this codebase: `fw_fetch_sd()` takes the path WITHOUT the `/sd` prefix (`/patterns/x.thr`, `/playlists/y.txt`) and prepends it, matching the reference clients | 45 s |
| `GET /sand_time` | `{epoch, synced, local, tz}` | 6 s |

### `/sand_status` fields consumed
```jsonc
{
  "state": "Idle|Run|Hold:0|Alarm|Home|Jog",  // split on ":" take [0]; Hold = paused
  "hostname": "dunetable",
  "file": "/sd/patterns/foo.thr",  // strip prefixes in order: /sd/patterns/, /patterns/, /sd/, /
  "running": true,
  "feed": 200,                     // mm/min (UI once mislabeled mm/s — it is mm/min)
  "progress": 0.42,                // fraction 0..1; negative = unknown → treat as 0
  "heap_largest": 20344,           // < 20000 ⇒ back off polling to 30 s
  "led": {"effect": "rainbow", "brightness": 255},  // live override of $LED/* NVS
  "playlist": {
    "active": true, "index": 0 /*0-based*/, "total": 12, "name": "Evening",
    "clearing": false,
    "pause_remaining": 812,        // s; -1 = not pausing
    "pause_total": 10800,          // s; -1 = unknown
    "next": "/patterns/x.thr",     // "" when unknown
    "last": "/patterns/y.thr"      // what's on the sand now; show during pauses
  }
}
```

### Actions (GET)
- `/sand_stop` (aborts clear+pattern+playlist), `/sand_pause` (feed-hold →
  `Hold:0`), `/sand_resume` — these + `/sand_status` are exempt from 503
  load-shedding.
- `/sand_home` (timeout 95 s), `/sand_goto?theta=<rad>&rho=<0..1>` (Idle only,
  95 s; center `rho=0`, edge `rho=1`).
- `/sand_feed?mm=<int>` — base feed mm/min, works mid-pattern. Options used by
  the UI: 50/100/150/200/300/500.
- `/sand_led?effect=&palette=&color=&color2=&brightness=&speed=&fgbright=&bgbright=&size=&align=&direction=&bg=`
  — any subset; colors are 6 hex digits **without `#`**.
- `/sand_time?epoch=<unix>&tz=<POSIX TZ>` — push once per connection after the
  first good status (board-local quiet hours depend on it). On ESP32, get time
  from SNTP first; TZ needs a config setting (no `/etc/localtime`).

### `$`-commands via `GET /command?plain=<urlencoded>`
Fire-and-forget; **never retry** (a timed-out `$Playlist/Run` may still have
run). Used: `$SD/Run=/patterns/<rel>` (no clear), `$Sand/Run=/patterns/<rel>
clear=<adaptive|in|out|none>`, `$Playlist/Mode=single|loop`,
`$Playlist/Shuffle=ON|OFF`, `$Playlist/PauseTime=<s>`,
`$Playlist/ClearPattern=<fw>`, `$Playlist/Run=<name-no-.txt>`,
`$Playlist/Skip`, `$Playlist/Autostart=<name|empty>`, `$Bye` (board reboot).
Clear-mode map: UI `adaptive/clear_center/clear_perimeter/none` → fw
`adaptive/in/out/none`; reading back, `sideway`/`random` → show as adaptive.

NVS writes (`$...=` settings) are idle-gated. Before `$Playlist/Run` with
settings: if not Idle → `/sand_stop`, then poll status every 0.5 s up to 15 s
until `state==Idle && !running`, then apply settings, then run.

### Resilience
- 503 `busy: low memory`: retry ×2, delay `0.3*2^n + rand(0,0.3)` s.
- Timeout/conn-error: 1 retry after 0.5 s — except `$`-commands (0 retries).
- Poll loop: 1 Hz base; drop tick if previous poll still in flight; interval =
  `max(base, last_response_ms)`; base 30 s when `heap_largest < 20000`;
  3 consecutive failures ⇒ show disconnected but keep polling (auto-recover).
  Real boards answer in 0.1 s–14 s mid-pattern; that's why timeout is 12 s.
- Friendly errors (timeouts stringify empty!): timeout → "The table didn't
  respond in time — it may be busy. Try again."; 401 → "The table rejected the
  password…"; conn → "Can't reach the table. Check that it's powered on…".

### File ops (ESP3D upload protocol)
- Upload: `POST /upload?path=<dir>` multipart with size field `<name>S` =
  `str(len(data))` and file part; use the Pi backend's convention (full SD path
  as part name). Timeout 60 s.
- `GET /upload?action=delete|createdir|rename&filename=&path=&dontlist=yes[&newname=]`.

### Playlists on SD
`/playlists/<name>.txt`, one SD-absolute path per line (`/patterns/x.thr`),
`#` comments allowed, duplicates allowed. Edit = rewrite whole file + upload.
Normalize: `/patterns/…` pass; `/sd/…` strip `/sd`; else prepend `/patterns/`.

## 2. mDNS discovery
- Browse `_http._tcp.local.`, keep only TXT `model=dune-weaver` (other TXT:
  `api=sandtable/1`, `ws=<port>`). Timeout 3 s.
- Prefer numeric IP over `.local` hostname; IPv4 only (boards have no AAAA and
  dual-stack lookups stall ~5 s). Dedupe by base URL.
- Auto-connect: saved URL if present on network, else the single/first find.
  Refresh lists only — switching tables is always an explicit tap.

## 3. Persistence (NVS here; `touch_settings.json` in the Pi app)
Keys: table_url, table_password (b64), screen_timeout (s, 0=never, default
300), pause_between_patterns (default 10800), playlist_shuffle (default true),
playlist_run_mode (default "loop"), playlist_clear_pattern (default
"adaptive"), dark_mode. Board NVS wins for playlist settings on connect
(re-read `Playlist/*` from `/sand_settings`).

Settings read on connect: `THR/Feed`, `Playlist/Mode|Shuffle|PauseTime|
ClearPattern|Autostart`; LED ring detection = any `LED/`-prefixed key exists;
then `LED/Effect|Palette|Brightness|Speed|Color|Color2|BallBright|
BallBgBright|BallSize|Align|BallBg|Direction`.

## 4. LED catalogue
Effects (index = firmware id):
`off static rainbow breathe colorloop theater scan running sine gradient
sinelon twinkle sparkle fire candle meteor bouncing wipe dualscan juggle
multicomet glitter dissolve ripple drip lightning fireworks plasma heartbeat
strobe police chase railway pacifica aurora pride colorwaves bpm ball`.
Palettes: `rainbow ocean lava forest party cloud heat sunset`.

Per-effect inputs: speed-only = lightning, police, pacifica, aurora, pride;
color-only = static(no speed), breathe, theater, scan, running, sine, sparkle,
candle, meteor, bouncing, drip, heartbeat, strobe; palette-only = rainbow,
colorloop, sinelon, twinkle, fire, juggle, multicomet, glitter, ripple,
fireworks, plasma, colorwaves, bpm; color+color2 = gradient, wipe, dualscan,
dissolve, chase, railway, ball. Unknown effect → show everything.

Rules: UI brightness 0–100 ↔ fw 0–255. Power = `effect != off`; remember the
last effect to restore on power-on (fallback `rainbow`). Ball tracker replaces
the effect; remember last non-ball effect. `/sand_status.led` is the live
truth mid-run ($LED/* NVS lags until the run ends). Ball params: `fgbright`
0–255, `direction` cw|ccw, `size` 1–30 in UI (fw clamps 1–200), `align`
0–359, `bg` effect name or static/off, `color2` for static bg, `bgbright`
0–255. Preset swatches (muted UI hex → sent hex): White e8e8e8→ffffff, Warm
d4a574→ffaa55, Red c45c5c→ff0000, Orange d4875c→ff8800, Yellow c9b95c→ffff00,
Green 5cb85c→00ff00, Cyan 5cb8b8→00ffff, Blue 5c7cc4→0000ff, Purple
8b5cc4→8800ff, Pink c45c99→ff00ff.

## 5. Pages (labels and semantics to preserve)
- **Browse**: grid of circular previews, search applies on Enter (not per
  keystroke), count in header, pattern detail = clear-mode chips (Adaptive
  clear / Clear from center / Clear from edge / Keep the sand; default
  adaptive) + "Weave this pattern" + "Add to playlist" (flips to "Added" ✓ for
  2 s).
- **Playlists**: list → detail with pattern list + remove, "Weave this
  playlist", shuffle chip (state lives on the board), Play order chips
  (Loop forever/Play once), rest-between chips 0s/1m/5m/15m/30m/1h/2h/3h/4h/
  5h/6h/12h, clear chips. Deleting: "Its patterns stay in your library."
- **Control**: connection card (status, password field, disconnect), tables on
  network (refresh + connect + manual address), table card (Home/Center/Edge,
  auto-play switch, Restart table = `$Bye`), screen card (sleep-after chips
  30s/1m/5m/10m/Never, night mode switch). "Shut down Pi" has no analogue.
- **Light**: power + brightness (0–100 step 5); appearance card (color/palette
  /speed 1–255 per effect table); effect chip grid (all minus `ball`); ball
  tracker card (dot color/brightness/direction/glow size/alignment + behind-
  the-dot background). Provider none → "No light ring is set up for this
  table…".
- **Now Playing**: ring progress from 12 o'clock; while resting the ring shows
  the pause countdown `1 - remaining/total` and the disc shows the *last*
  pattern (what's on the sand) with eyebrow "Up next"; playlist line
  `<name> · <i+1> of <total>[ · clearing]`; progress line `NN% woven` or
  `H:MM:SS until the next pattern`; transport Pause/Resume (enabled iff a file
  is loaded), Stop, Skip (flex 3:2:2); speed segmented control
  50/100/150/200/300/500 mm/min.
- Header on every page: connection dot + table name (tap → rescan + switch-
  table popup); UI stays fully usable while disconnected.
- First touch after ≥2 s idle is swallowed (wake-up touches report bogus
  coordinates) — also the hook for screen-sleep wake.

## 6. Previews (.thr → image)
- `.thr` = text lines `theta_radians rho_0..1`; theta signed/unbounded
  (multi-turn), rho clamp to [0,1]; skip blanks/`#`; files run 2k–36k points.
- Transform: `x = cx + rho*R*cos(theta)`, `y = cy + rho*R*sin(theta)`.
- Look: transparent corners, dish fill `#1b1712` + ring `#3e362c`, stroke
  sand `#d8b578`, stroke ~2 px at 512 (Pi renders 2048 supersampled + LANCZOS;
  on-device: direct draw at target size, decimate points < ~0.5 px apart).
- Pi caches by `sha1(file)[:10]` + renderer version; board serves files at
  30–60 KB/s so cache aggressively (LittleFS `storage` partition) and fetch
  with concurrency 1, render off the LVGL task.
- Distinguish "definitively unrenderable" (cache the failure) from "transient
  fetch failure" (retry ×3, 10 s apart, then leave uncached).

## 7a. Panel-local pattern SD card (ESP32 addition, 2026-08-25)

Not in the QML reference — the panel has its own TF slot (SPI: MOSI=11,
SCK=12, MISO=13; CS = CH422G EXIO4 held low all session, sdspi runs with no
CS pin). A user-prepared card makes Browse instant and network-free, the
dune-weaver-mobile way:

- `/patterns.json` — manifest, same JSON-array format as `/sand_patterns`
  (which itself serves the table card's `/patterns/index.json` verbatim when
  present). SD manifest wins; `/sand_patterns` is the no-card fallback.
- `/previews/<key>.bin` — raw little-endian RGB565 300×300 (180,000 B),
  `key` = pattern basename lowercased incl. ".thr" (mobile's previewKey:
  basename matching makes previews cross-table). Prepared by
  `tools/make_pattern_sd.py`; byte-compatible with the LittleFS render cache.
- Preview lookup order: RAM LRU → SD tile → LittleFS cache → fetch+render.
- No card ⇒ Browse shows a complaint banner and falls back to the network
  path; a card inserted after boot is picked up on the next Browse refresh
  (weak `sdcard_remount` hook in sd_catalog).
- Browse builds cards in chunks of 48 behind a "Show more" tile: real tables
  carry 1000+ patterns and a card per pattern exhausts internal RAM
  (SPIRAM_MALLOC_ALWAYSINTERNAL=4096 puts every small LVGL alloc there),
  starving the WiFi driver — the 2026-08-25 red-dot bug.

## 7. Fonts / icons
Target fonts (bundled in the Pi repo's `fonts/`): Outfit Regular/Medium/
SemiBold + Material Icons Round. Convert with `lv_font_conv` at the scaled
sizes; icon subset (28 glyphs): add e145, adjust e39e, arrow_back e2ea,
brightness e3ab, check e5ca, circle ef4a, close e5cd, delete e872,
expand_more e5cf, home e88a, light_mode e518, lightbulb e0f0, music_note
e405, pause e034, play_arrow e037, play_circle e1c4, playlist_play e05f,
power e8ac, queue_music e03d, radio_unchecked e836, refresh e5d5, restart
f053, search e8b6, shuffle e043, skip_next e044, stop e047, tune e429,
wifi e63e.
