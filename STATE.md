# STATE — 2026-08-25

Session context snapshot. Durable rules live in `CLAUDE.md`; the protocol/UX
contract lives in `docs/PORTING_NOTES.md`. This file is point-in-time.

## Where things stand

Increment 2 is flashed and stable on the board. Repo is **git-initialized with
zero commits** (initial commit deliberately not made — Tuan reviews first).

### Verified on hardware (2026-08-25)

- Boot chain clean: NVS settings → CH422G/I2C → RGB panel 1024×600 @ 21 MHz →
  GT911 touch (ID 911) → LittleFS `storage` (10 MB) → WiFi STA → both job
  workers → state poll task.
- WiFi provisioning from the Control page worked end-to-end; credentials
  persisted (`The Bears' Wi-Fi Network`), panel got 192.168.68.156.
- mDNS discovery works: found 4 tables — DWG @ .163, DWMAX @ .123,
  DWMP2 @ .153, DWPro @ .115 (multi-table household → auto-connect correctly
  waits for a manual pick).
- 36 automated tab switches with no crash after the LVGL stack fix
  (`-DUI_DEBUG_TAB_CYCLE` soak).

### Crash fixed this session

LVGL task stack overflow (default 7168 B) rendering Control/Playlists/Now
Playing. Symptoms: StoreProhibited in FreeRTOS portasm, backtraces full of
0xa5a5a5a5, double-core panics. Fix: `port_cfg.task_stack = 16384` in
`src/board/display.c` + `FREERTOS_WATCHPOINT_END_OF_STACK`. Two earlier boot
blockers also fixed: XIP-from-PSRAM (flash ops vs bounce-buffer ISR) and LVGL
CLIB malloc (builtin 64 KB pool OOM → `while(1)` assert). All three are
documented as rules in CLAUDE.md.

### NOT yet exercised against a real table

Table pick/connect, status polling against real firmware, pattern browse with
previews (board serves .thr at 30–60 KB/s; first render slow, then FS-cached),
weave + clear modes, playlists CRUD/run, transport, speed, LED page, ball
tracker, password-protected boards (401 path), auto-play read-back. Next
session should start here: pick a table on Control → walk every page.

## BUG (found 2026-08-25, live-tested against a real table): browse grid
## exhausts internal RAM → WiFi starves → red dot + crawling fetches

First real-table session: picking a table made pattern loading crawl and the
header dot go red while requests still (slowly) succeeded. Serial capture
showed the smoking gun: `wifi:m f null` at ~10 Hz (WiFi driver can't allocate
frame buffers) + `mdns_receive: Cannot allocate memory` — **internal** heap
exhaustion (the "free heap: 2.9 MB" in those lines is PSRAM, not internal).

Cause chain: every real table carries ~1000+ patterns (measured: DWG 1031,
DWMAX 1080, DWMP2 1224, DWPro 946). `page_browse.c` `rebuild_grid()` creates
a card per pattern — no virtualization — and with
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` every small alloc (LVGL objects,
cJSON nodes during /sand_patterns parse, fw_client's cached path strings ×2
copies) lands in internal RAM. Several thousand sub-4 KB allocations later the
WiFi/lwIP driver can't get buffers → status polls fail → 3-streak →
CONN_LOST → red dot (the dot is truthful; the panel's own networking is
degraded, the table is fine). Grid build also runs under lvgl_port_lock →
seconds of UI freeze. Previews themselves are correctly lazy/visible-only,
but every /sd fetch shares one s_wire_mutex with the poll, at 30–60 KB/s.

FIXED same day (flashed + verified): Browse now builds 48 cards per chunk
behind a "Show more" tile (no more 1000-card internal-RAM bomb), and the SD
card scheme below removes the network preview path entirely when a card is
in. Verified on hardware: DWMP2's 1224 patterns load with no wifi:m-f-null
storm. Still open: LVGL allocs generally landing internal (ALWAYSINTERNAL);
revisit only if pressure reappears.

## Pattern SD card scheme (added 2026-08-25, Tuan's direction)

Like dune-weaver-mobile: manifest + pre-rendered previews live on a microSD
the user preps and pops into the panel's TF slot. Contract in PORTING_NOTES
§7a; card prep via tools/make_pattern_sd.py (Pillow render, firmware-identical
look, ~200 MB for the full 1232-pattern library). Browse: SD manifest first
(works offline, loads at page-create), /sand_patterns fallback + complaint
banner when no card; previews RAM → SD → LittleFS → fetch+render. Verified:
in the UI sim end-to-end (1232 patterns from SD manifest, zero network
preview fetches); on hardware only the NO-card path so far (graceful
ESP_ERR_TIMEOUT, banner, table fallback) — a real prepared card in the slot
is still untested. Hot-insert: next Browse refresh remounts (weak
sdcard_remount hook). New board facts: SD SPI MOSI=11 SCK=12 MISO=13, CS =
CH422G EXIO4 active-low held for the session (board.c now tracks the EXIO
latch byte; sdspi runs CS-less).

## Table simulator (added 2026-08-25)

`tools/table_sim.py` (stdlib-only) mocks the full firmware API + mDNS so the
panel can be exercised without a real table — status/state machine (Run/Hold/
Home), playlist engine with clearing + shuffle + rest pauses, ETag/304 on
`/sand_patterns`, throttled `/sd` (~45 KB/s), `$`-commands incl. `$Bye`
(4 s dead-socket "reboot"), ESP3D upload for playlist CRUD, `--password` for
the 401 path, `--heap-low` for the 30 s poll backoff. Verified via curl:
discovery TXT, run/pause/resume/stop, playlist run/skip/clearing, 304, and
throttle timing all match the contract. Not yet exercised from the panel.

## UI simulator (added 2026-08-25)

`sim/` runs the firmware UI on the Mac — see CLAUDE.md for build/run. The
device sources compile unmodified; only board/, wifi.c, discovery.c are
faked. Verified end-to-end on 2026-08-25: boot → stub discovery → auto-connect
to the table sim → connect edge (18 settings, LED ring, time push) → 1232
patterns + playlists loaded → 1 Hz status polls → throttled preview fetches.
Note the sim showed "loaded 1232 patterns" with no visible stall — the
internal-RAM bug above is device-only, which is exactly why hardware still
gates release.

## Build/debug quick reference

- `pio run -t upload` then serial via `/dev/cu.usbmodem*` (USB CDC; UART0 is
  physically wired to RS485).
- Crash repro hands-free: `-DUI_DEBUG_TAB_CYCLE` in `platformio.ini`
  build_flags (auto-cycles tabs every 4 s with heap telemetry).
- Decode panics:
  `~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/waveshare-5b/firmware.elf <addrs>`
- Changing `sdkconfig.defaults` requires `rm sdkconfig.waveshare-5b` first.

## How this code was built (for archaeology)

Increment 2 was a 12-agent ultracode workflow: 10 parallel implementers
against contract headers (net/, app/, render/, pages), then protocol +
concurrency auditors. Audit fixes applied during integration: preview-cache
refcounting (use-after-free), poll-vs-disconnect generation guard, board-NVS
playlist seeding on connect, settings mutex, jobs fast lane (transport never
queues behind slow fetches), autostart control with read-back, SD-path
convention (`fw_fetch_sd` prepends `/sd`). One workflow agent fabricated its
completion report (fw_client.c was never written) — caught by the auditor,
re-run with proof-of-write required.

## Deferred backlog (increment 3+)

1. DONE 2026-08-25: Outfit + Material Icons Round shipped (`tools/gen_fonts.sh`
   → `src/ui/fonts/outfit_{18,21}.c`, `outfit_sb_{26,36}.c`, each merged with
   the §7 Material subset + LVGL's FontAwesome symbols so LV_SYMBOL_* and the
   keyboard's built-in glyphs keep working). All pages now use `TH_ICON_*`
   Material macros (nav bar matches the reference glyph-for-glyph). Same
   session: themed keyboard (`ui_keyboard_create`, was stock light), circular
   preview clipping (clip_corner on the three preview surfaces), 4-column
   browse grid (CARD_W 230 — matches reference density AND physical size),
   header chevron, search pill w/ magnifier. Verified side-by-side against
   the QML reference running on the Mac against the table sim (PySide6 venv +
   self-screenshot hook in scratchpad; `sim/shot.py` snapshots the LVGL sim).
   Kept deliberately different: speed label says MM/MIN (reference's MM/S is
   the documented mislabel).
2. DONE 2026-08-25: screen sleep + touch-to-wake (`src/ui/screen_sleep.c`).
   1 Hz LVGL timer vs `lv_display_get_inactive_time`; sleep = backlight off +
   opaque-black shield on `lv_layer_sys`; wake = backlight on at PRESSED,
   whole gesture swallowed until RELEASED (shield covers everything, so
   GT911's bogus wake coordinates can't hit a widget). Reads the setting live
   — Control-page chips apply immediately. Verified in the sim end-to-end
   (5 s timeout: sleep, wake-on-click with swallow, re-arm); on device the
   flash boots clean but sleep/wake awaits an idle window + a finger — set
   the 30 s chip on Control to test. NOT implemented: the reference's
   general "swallow any first touch after 2 s idle" (only the sleep-wake
   swallow) — add only if bogus taps show up on hardware after short idles.
3. Live night/day retheme (Control switch persists but applies on restart;
   widgets bake colors at creation).
4. Preview cache keying: path-hash + size + no renderer version byte —
   re-uploaded patterns serve stale tiles (PORTING_NOTES wants content hash).
5. Playlist row "<n> patterns" counts: capped at 12 rows, first fetch failure
   aborts the pass (board has no playlist manifest route).
6. Quiet hours ($Sands/*) UI — board-side feature, not in the reference touch
   UI either; needs the TZ push (fw_sync_time currently sends epoch only).
7. OTA partition scheme (single factory app today).

## Environment facts

- Board: Waveshare ESP32-S3-Touch-LCD-5B on USB (`/dev/cu.usbmodem*`),
  ESP32-S3 rev v0.2, 16 MB quad flash, 8 MB octal PSRAM.
- Reference app (feature parity target):
  `/Volumes/SSD/projects/dune-weaver-pi/dune-weaver-touch` (PySide6/QML).
- Waveshare demo bundle + wiki extracts were the pinout/timing source; pins in
  `src/board/board.h`.
