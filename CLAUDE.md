# dune-weaver-touch (ESP32-S3)

LVGL firmware for the Waveshare ESP32-S3-Touch-LCD-5B that reimplements the
PySide6/QML touch app at `/Volumes/SSD/projects/dune-weaver-pi/dune-weaver-touch`.
Read `STATE.md` first for the current status, validation state, and backlog.
That app is the reference for every feature, label, and behavior; `docs/PORTING_NOTES.md`
holds the distilled contract (firmware HTTP API, page specs, constants). When in
doubt, read the QML source — do not invent behavior.

## Commands

- Build: `pio run` · Flash: `pio run -t upload` · Monitor: `pio device monitor`
- Console is native USB CDC (`/dev/cu.usbmodem*`); UART0 is physically wired to RS485.
- First build downloads managed components into `managed_components/` (gitignored).
- Table sim: `python tools/table_sim.py` — mock table on the LAN (mDNS `DWSIM`,
  port 8080, TXT `model=dune-weaver`) implementing the PORTING_NOTES API against
  the real dune-weaver-pi pattern library, `/sd` throttled to ~45 KB/s like a
  real board. Flags: `--fast`, `--password <pw>` (401 path), `--heap-low`
  (poll-backoff path), `--port/--name` for a second instance. Playlists/uploads
  land in `tools/sim_data/` (gitignored).
- Pattern SD card: `python tools/make_pattern_sd.py --out /Volumes/<card>`
  (or `--out sim/simfs/sdcard` for the UI sim; `--from-table <url>` to mirror
  a specific table's catalog; `--from-bundle <dir>` to resample a
  dune-weaver-website Pattern Manager export's .thr.webp alpha masks —
  nicer strokes than the built-in render). Layout + contract: PORTING_NOTES
  §7a; the firmware only reads `/patterns.json` + `/previews/*.bin` at card
  root, so a PM export under `/patterns/` coexists untouched. Re-running is
  incremental — it rewrites only tiles that are missing or the wrong length,
  so a format change repairs a card in place. Copy the bundle shards off the
  card first; reading masks and writing tiles through one reader thrashes.
- UI sim (no board needed): `cmake -S sim -B sim/build && cmake --build
  sim/build -j && ./sim/build/dwt_sim` — the firmware's ui/app/render/net
  layers compiled UNMODIFIED against LVGL 9 + SDL2 in a 1024×600 window;
  `sim/shim/` fakes ESP-IDF (pthread FreeRTOS, POSIX esp_http_client,
  file-backed NVS, /storage → `sim/simfs/`), `sim/stub/` fakes WiFi/mDNS.
  Tables via `DWT_SIM_TABLES="Name=url,..."` (default DWSIM=127.0.0.1:8080 —
  run the table sim). Device-specific bugs (internal-RAM exhaustion, stack
  sizes, flash-cache faults) do NOT reproduce here — the sim is for UI/protocol
  work, hardware still gates release.
- Preview pipeline check: `DWT_SIM_PREVIEW_SELFTEST=<pattern.thr>
  ./sim/build/dwt_sim` dumps that pattern's tile at every size the UI asks for
  and exits; `python sim/check_tiles.py out.png` renders the dumps. Use it on
  any card-format change — the 300 px path (Now Playing, Browse detail) is
  otherwise only reachable by tapping, so it silently goes unverified.

## Hardware facts (verified against Waveshare wiki + demo, do not "fix")

- 1024×600 RGB565 parallel panel, PCLK 21 MHz, porches H 145/170/30, V 23/12/2,
  `pclk_active_neg=1`. Pin map lives in `src/board/board.h`.
- 16 MB **quad** flash (eFuse), 8 MB **octal** PSRAM → `FLASHMODE_QIO` +
  `SPIRAM_MODE_OCT`. Framebuffers live in PSRAM; bounce buffers are mandatory.
- `CONFIG_SPIRAM_XIP_FROM_PSRAM` (+FETCH_INSTRUCTIONS/RODATA) is REQUIRED:
  any flash op (NVS write, LittleFS) disables the cache, and the bounce-buffer
  ISR reading the PSRAM framebuffer then cache-faults (boot Guru Meditation).
- LVGL must use `CONFIG_LV_USE_CLIB_MALLOC` (+`SPIRAM_USE_MALLOC`): the
  builtin 64 KB pool can't hold the five-page widget tree, and LVGL's OOM
  assert spins in `while(1)` — symptoms are a task-watchdog storm pinned at
  one `lv_obj_create` backtrace.
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` is REQUIRED: at the old 4096, the
  widget tree + a 1200-pattern cJSON parse exhausted internal RAM to <1 KB
  and the WiFi driver storm-failed beacon buffers (`wifi:m f null` at 10 Hz,
  fetches dead). Watch the 10 s `app: heap:` heartbeat when adding anything
  that allocates internal RAM (task stacks, queues).
- The LVGL task needs a 16 KB stack (`port_cfg.task_stack` in display.c; the
  esp_lvgl_port default 7168 overflows rendering the deeper pages). Overflow
  symptoms: corrupted backtraces containing 0xa5a5a5a5 and StoreProhibited in
  FreeRTOS portasm. `FREERTOS_WATCHPOINT_END_OF_STACK` is enabled so any
  recurrence faults precisely. To reproduce render crashes hands-free, build
  with `-DUI_DEBUG_TAB_CYCLE` (auto-cycles tabs every 4 s).
- CH422G IO expander has no register pointer — each function is an I2C address:
  write `0x01` to addr `0x24` (all-output mode), then a bitmask to addr `0x38`.
  EXIO1=TP_RST, EXIO2=DISP/backlight, EXIO3=LCD_RST, EXIO4=SD_CS. Backlight is
  on/off only (no PWM).
- GT911 latches I2C address 0x5D only if INT (GPIO4) is held low through reset —
  that's why `board_init()` drives GPIO4 as an output during the reset dance.

## Architecture

- `src/board/` — board bring-up (I2C, CH422G, RGB panel, GT911, esp_lvgl_port).
  Touch is a dedicated 10 ms poll task + queue drained by the LVGL read
  callback (`continue_reading`), NOT `lvgl_port_add_touch`: sampling from the
  LVGL task loses the gesture during slow frames and a flick then registers
  as a card CLICK (see STATE.md 2026-08-25).
- `src/ui/` — `theme.[ch]` = design tokens (ported from `ThemeManager.qml`,
  1.5× physical scale for the 237 PPI panel), `ui.[ch]` = root layout + shared
  widget recipes, `pages/` = one file per tab.
- Planned: `src/net/` — WiFi, mDNS discovery, firmware HTTP client (increment 2).
- All LVGL calls outside the LVGL task must hold `lvgl_port_lock()`.

## Rules

- The table's firmware API is stateless and multi-client: poll `/sand_status`
  to confirm; never trust a command's return value. Never retry `$`-commands.
- Design tokens come from `theme.h` — no hardcoded colors in pages. Icons are
  Material Icons Round via the `TH_ICON_*` UTF-8 macros (LV_SYMBOL_* also
  still renders — FontAwesome is merged into the fonts for the keyboard).
- Fonts are Outfit + Material Icons Round + LVGL FontAwesome, merged per size
  in `src/ui/fonts/` — regenerate with `tools/gen_fonts.sh` (needs node);
  requires `-DLV_LVGL_H_INCLUDE_SIMPLE` (set in platformio.ini + sim CMake).
- Visual parity is checked against the reference app running on the Mac:
  `sim/shot.py out.png` self-screenshots the running UI sim (no macOS screen
  permissions needed); the QML reference runs against the table sim via a
  PySide6 venv (see STATE.md).
- Keep the UI usable when disconnected — only the header dot changes state.
- **The panel never renders or stores previews.** Tiles come off the pattern
  TF card only (`thr_preview` = RAM LRU → SD, nothing else); rendering lives
  in `tools/make_pattern_sd.py`. There is no `storage` partition. No card
  just means placeholder dishes + the Browse banner — do not "restore" a
  fetch-and-render fallback (PORTING_NOTES §6 says why it went).
- **The card is ONE folder at ONE size**: `/previews/<key>.bin`, a 300×300
  4-bit alpha mask, exactly 45,000 B. No `previews@<N>/`, no second format,
  no fallback. A tile is coverage only — the dish/ring/sand colours are theme
  tokens (`th.preview_*`) and thr_preview composites them at load. Retheming
  therefore needs `thr_preview_clear_ram()`, not a card re-prep.
- Still request tiles at the size the widget displays: thr_preview resamples
  the mask to that size, so a src-size ≠ widget-size image would add a
  per-frame LVGL transform on top for nothing.
- **Never `clip_corner` a preview tile.** Tiles paint their own corners the
  colour of what they sit on (`thr_preview_get`'s `corner`), so `radius`
  alone is enough. Clipping routes every child through two ARGB8888 layers +
  a mask per frame (lv_refr.c) — the single biggest scroll cost there was.
- Browse card budget: `1024 - 2*PAGER_W - 2*TH_SPACE_LG >= 4*CARD_W +
  3*TH_SPACE_MD`. Forget the grid's own pad_hor and a page wraps to 3 rows
  and spills under the nav bar.
- **Nothing scrolls by dragging.** Dragging redraws all 1024×600 per frame on
  this panel. `plain()` clears `LV_OBJ_FLAG_SCROLLABLE` in every page; give
  any overflowing region `ui_page_stepper(parent, scroller)` for an Up/Down
  column instead. Browse instead rebuilds a page of cards, which also bounds
  tile memory — keep its card count derived from the measured grid height.
- Anything that pins a tile must bound how many are resident (182 px = 66 KB
  each). A page's worth is freed by the next `rebuild_grid`;
  `-DUI_DEBUG_PREVIEW_SCROLL` soak-tests that it stays flat.
- **A bare `lv_obj` is CLICKABLE by default** — `plain()` clears SCROLLABLE, not
  that. So a purely decorative container dropped inside a row you made
  clickable (a text column, an icon wrapper) *swallows* the tap: it is hit
  first, has no handler, and LVGL does not bubble unless you ask it to, so
  the row never fires and nothing happens at all. Clear
  `LV_OBJ_FLAG_CLICKABLE` on every `plain()` that sits on top of a touch
  target. Symptom: the middle of a widget is dead while its margins work —
  and no event fires anywhere, which reads like the input stack is broken
  rather than like a hit-test that stopped one level too early.
- **Batch widget updates under ONE `lvgl_port_lock`.** `full_refresh` makes any
  invalidation re-render all 1024×600, and the panel is 24 Hz, so N separate
  attaches cost N full redraws — measured at ~200 ms EACH, which is 4× the tile
  load it was hiding behind. LVGL coalesces everything invalidated while the
  lock is held into one refresh. This is why Browse loads a page per job.
- **`ESP_LOGD` does not exist on device** (`CONFIG_LOG_MAXIMUM_LEVEL=3` compiles
  it out) — debug timers you add will only ever fire in the sim. Gate device
  measurements behind a build flag that logs at INFO, like `-DPV_DEBUG_TIMING`.
  Measure on hardware before optimising; the sim has no 24 Hz panel and no PSRAM
  latency, so it cannot show you where the time actually goes.
- `PV_SD_SIZE_PX` (thr_preview.c) and `make_pattern_sd.py --size` must agree.
  The file LENGTH is the only format check there is, so a mismatch is not an
  error message — it is 1200 silent placeholder dishes. Change them together.
- **Read SD files with POSIX `open`/`read` and a big chunk, never `fread`.**
  newlib refills through the FILE's own small buffer, so FATFS sees ~1 KB
  reads and you get ~450 KB/s instead of ~1300 KB/s. Measured 2026-08-26.
  Read into internal RAM and memcpy to PSRAM: a PSRAM destination makes
  `sdmmc_read_sectors` fall back to single-block reads (the S3 does not set
  `SOC_SDMMC_PSRAM_DMA_CAPABLE`).
