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
300 in the Pi app — **this panel defaults to 60**, the "1 m" chip, since it is
an always-on wall panel rather than a windowed kiosk app; Tuan, 2026-08-27),
pause_between_patterns (default 10800), playlist_shuffle (default true),
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

**The panel does not render previews.** Since 2026-08-26 tiles come off the
pattern TF card only (§7a) — no rasterizer, no `.thr` fetching, no flash
cache, no `storage` partition. The rules below describe the *reference*
pipeline and the card-prep tool (`tools/make_pattern_sd.py`), which is where
rendering now lives.

- `.thr` = text lines `theta_radians rho_0..1`; theta signed/unbounded
  (multi-turn), rho clamp to [0,1]; skip blanks/`#`; files run 2k–36k points.
- Transform: `x = cx + rho*R*cos(theta)`, `y = cy + rho*R*sin(theta)`.
- Look: dish fill `#1b1712` + ring `#3e362c`, stroke sand `#d8b578`, stroke
  ~2 px at 512 (Pi renders 2048 supersampled + LANCZOS; the card tool
  supersamples ×4 + LANCZOS to match). The stroke **scales with tile size** —
  a flat 2 px at 204 is 2.5× the spec and closes the gaps between adjacent
  passes, turning fine patterns into solid discs.
- Since 2026-08-26 those three colours live in the FIRMWARE, as
  `th.preview_dish/ring/sand` (night values are the hexes above, so nothing
  shifted). Card tiles are bare coverage masks; the tool renders geometry
  only. The dish rim is antialiased on device with a 1 px ramp in
  `base_tile_get` rather than by supersampling — a hard edge there reads as a
  visibly jagged circle (measured 32/255 along the whole circumference).
- Why the panel gave it up: the board serves `.thr` at 30–60 KB/s, so a
  1200-pattern library took hours to populate and shared one wire mutex with
  the status poll, and the LittleFS tile cache spent 10 MB of flash storing
  what the card already ships.

## 7a. Panel-local pattern SD card (ESP32 addition, 2026-08-25)

Not in the QML reference — the panel has its own TF slot (SPI: MOSI=11,
SCK=12, MISO=13; CS = CH422G EXIO4 held low all session, sdspi runs with no
CS pin). A user-prepared card makes Browse instant and network-free, the
dune-weaver-mobile way:

- `/patterns.json` — manifest, same JSON-array format as `/sand_patterns`
  (which itself serves the table card's `/patterns/index.json` verbatim when
  present). SD manifest wins; `/sand_patterns` is the no-card fallback.
- `/previews/<key>.bin` — a 300×300 **4-bit alpha mask**, exactly 45,000 B.
  `key` = pattern basename lowercased incl. ".thr" (mobile's previewKey:
  basename matching makes previews cross-table). Prepared by
  `tools/make_pattern_sd.py` — from a local `.thr` library, or with
  `--from-bundle` from a dune-weaver-website Pattern Manager export
  (previews.json + shard-*.zip of 512 px `.thr.webp` alpha masks).
  **ONE folder, ONE size.** The per-size `previews@<N>/` directories are gone
  (2026-08-26): the panel resamples the single mask to whatever a widget
  displays at, and doing that on one 8-bit channel is cheap (~3 ms for
  300 → 160) where resampling three packed RGB565 channels was ~20.
- A tile carries **coverage only** — no colour, no dish, no ring. The panel
  builds the dish from `th.preview_dish/ring/sand` and composites the mask
  through it (`thr_preview.c` `base_tile_get` + `composite_tile`). Two
  consequences worth keeping straight: retheming needs no card re-prep (it
  needs `thr_preview_clear_ram()`, which drops the cached dish bases), and
  the card's look is now a firmware decision, not a card decision.
  Nibble order is low-first: byte i holds pixel 2i low, 2i+1 high; 0 = bare
  dish, 15 = full sand. Row-major, no header, no padding.
- **The file length IS the format check.** There is no magic number and no
  version field: `sd_preview_load` `fstat`s and rejects anything that is not
  `size*size/2`. A card written for older firmware (300 px RGB565 =
  180,000 B) therefore shows placeholder dishes everywhere; the firmware logs
  one warning naming the expected length rather than 1200 silent misses.
- Preview lookup order: RAM LRU → the card's mask, resampled + composited.
  That is the whole list. There is no fallback and no second size.
- Measured: 45,000 B beats the old 180,000 B master AND the 51,200 B 160 px
  RGB565 tile, so this is smaller than either previous layout while feeding
  every widget at full quality. 4-bit quantisation lands within 6/255 of an
  8-bit composite — under the RGB565 step of 8 the panel quantises to anyway.
- **Tiles paint their own corners, so nothing is ever masked.** The area
  outside the dish is filled with the colour of whatever the image sits on
  (`thr_preview_get`'s `corner`: `th.surface` in the grid, `th.bg` for the
  detail overlay and Now Playing), and the widgets set `radius` but NOT
  `clip_corner`. This matters a lot: LVGL renders a clipped object's children
  through two ARGB8888 layers plus a rounded-rect mask every frame
  (`lv_refr.c` — for a circle the "middle" band is empty, so the whole tile
  goes through it), i.e. ~166 KB of 32-bit layer traffic per card per frame.
  Corner colour is part of the LRU key. `ui_rgb565()` does the conversion;
  the match is exact because the draw buffer is RGB565 too.
- Errors are two kinds: `ESP_ERR_NOT_FOUND` (card in, no tile for this
  pattern) is permanent — show the plain dish, never retry.
  `ESP_ERR_INVALID_STATE` (no card) is retryable after a 10 s backoff.
- No card ⇒ Browse shows a complaint banner, still lists patterns from
  `/sand_patterns`, and every card keeps its placeholder dish. A card
  inserted after boot is picked up on the next Browse refresh (weak
  `sdcard_remount` hook in sd_catalog).
- **Browse is PAGED, not scrolled** (2026-08-26, deliberate deviation from
  the QML reference's scrolling grid). `full_refresh` + `avoid_tearing` means
  every frame of a drag redraws all 1024×600, so scrolling is the most
  expensive thing the UI can do while a static page costs nothing; a page tap
  spends one redraw instead. It also removes momentum throw, the catch-tap
  that used to select a pattern mid-slide, and any need to unload off-screen
  tiles — a page is 8 cards (4×2) and rebuilding frees them.
  Up/Down buttons sit in a `PAGER_W` column down the RIGHT of the grid (not
  in the header): they page a vertical list, so they read as up/down, and
  putting them beside the content keeps them under the thumb. The header
  carries only the "1-8 of 1232" range label.
  Rows are measured from the grid's real height, not hardcoded, because the
  SD-complaint banner steals a row's worth of space when it is up.
  `-DUI_DEBUG_PREVIEW_SCROLL` steps pages and logs tile residency.
  Tiles load a PAGE per job and attach under ONE `lvgl_port_lock` so LVGL
  coalesces the invalidations into a single redraw — attaching them one at a
  time cost ~200 ms each (measured), four times the tile load it hid behind.
  Once the page is complete the loader warms page N+1 into the RAM LRU
  (`PV_KIND_PREFETCH`: load, then release immediately so it stays cached
  unpinned), which makes a Next tap one redraw instead of a page of reads.
  Visible cards always win the jobs lane over a prefetch.
- Header and nav bars are 72 px each (were 90/96), which is what buys the
  160 px preview: 600 − 72 − 72 = 456. Anything placed in a header must fit
  that — the search pill and round buttons are 56.
- **NOTHING in this UI scrolls by dragging** (2026-08-26). Every page's
  `plain()` clears `LV_OBJ_FLAG_SCROLLABLE` (LVGL sets it on every object by
  default), and any region whose content overflows gets `ui_page_stepper()`:
  it keeps the flag but sets `LV_DIR_NONE` — the indev then finds no scroll
  target (`lv_indev_scroll.c:327,343`) while `lv_obj_scroll_by` still works,
  since that has no flag check. The stepper builds an Up/Down column beside
  the content and steps one viewport per tap, dimming at the ends and
  following content changes (CHILD_CHANGED/SIZE_CHANGED). Used by: Control
  body, both Light columns, all three Playlists regions, and the two modal
  pickers. Browse is different — it rebuilds a page of cards, which also
  bounds tile memory.
- Sim caveat: `sim/shim/sim_remap.h` must remap every file call the device
  code uses. It rewrites `fopen`/`open`/`stat`/`unlink`/`rename`/`mkdir`;
  add to it when new ones appear, or the sim silently reads the host root.
- Browse never builds a card per pattern. Real tables carry 1000+, and one
  card each exhausted internal RAM and starved the WiFi driver — the
  2026-08-25 red-dot bug. Two independent fixes stand: paging (a page is 8
  cards, never more) and `SPIRAM_MALLOC_ALWAYSINTERNAL=0`, which moves the
  LVGL widget tree and cJSON parse nodes to PSRAM. The interim "Show more"
  48-card chunking is gone — paging replaced it.

## 7b. What the web Pattern Manager must emit (OPEN — not built yet)

Today a card is prepped on a Mac: the dune-weaver-website Pattern Manager
exports 512 px `.thr.webp` alpha masks in shard zips, and
`tools/make_pattern_sd.py --from-bundle` composites them into the §7a
layout. **The goal is for the web interface to emit a panel-ready card
directly**, so a user never runs a Python script. Whatever generates it must
satisfy the §7a contract exactly — the firmware does zero decoding, so a
wrong byte count is a silent missing tile, not an error message:

- **One directory, `previews/`, one size.** Not `previews@<N>/` — those were
  removed 2026-08-26. The panel resamples the single mask to whatever a
  widget needs.
- **Packed 4-bit alpha, no header, no compression.** 300×300 → exactly
  45,000 B. `sd_preview_load` `fstat`s and rejects any other length as
  `ESP_ERR_NOT_FOUND`, so the length is the entire format negotiation: get it
  wrong and the panel shows placeholder dishes with one log line. No PNG, no
  WebP, no BMP — the panel has no decoder and deliberately ships none (§6).
  Nibble order low-first (byte i = pixel 2i low, 2i+1 high), row-major,
  0 = bare dish, 15 = full sand.
- **Coverage only — no colour, no dish, no ring, no background.** The panel
  owns the look (`th.preview_dish/ring/sand`). An exporter that bakes colour
  in would break night/day retheming and would not match the panel's palette
  anyway. Geometry is the one thing that must line up: the pattern's rho=1
  maps to the dish edge at radius `size/2 - margin`, `margin = size*12/512`,
  so the mask occupies a `size - 2*margin` inset and is zero outside it.
  Stroke width **scales with tile size** (~2 px at 512); a flat stroke turns
  small tiles into solid discs.
- **Key = pattern basename, lowercased, `.thr` kept**, `.bin` appended:
  `sub/Star.thr` → `star.thr.bin`. Basename-only is deliberate (mobile's
  `previewKey`) so a preview follows a pattern across folders and tables. It
  also means two patterns sharing a basename share one tile — 65 of Tuan's
  1232 do, and that is accepted behaviour, not a bug.
- Skip macOS `._*` AppleDouble files when writing to FAT — they collide with
  real keys and the tool filters them for this reason.

Good news for the web side: the Pattern Manager's existing 512 px
`.thr.webp` exports are **already alpha masks of exactly this kind**. Turning
one into a tile is resample-into-the-inset then pack to 4 bits — strictly
less work than the old RGB565 compositing. `render_bundle_tile()` in
`tools/make_pattern_sd.py` is ~10 lines and is the reference implementation
to port.

Still open, and worth doing before this ships to users: packing all tiles
into one indexed blob would remove the per-file `open()` in a 1167-entry FAT
directory (measured 1–15 ms each), which is now a real share of the per-tile
cost. That changes the layout again, so decide it before asking anyone to
re-prep a 1232-pattern card. Analysis: STATE.md "Preview load speed: where
the time actually goes".

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
