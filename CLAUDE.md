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
  a specific table's catalog). Layout + contract: PORTING_NOTES §7a.
- UI sim (no board needed): `cmake -S sim -B sim/build && cmake --build
  sim/build -j && ./sim/build/dwt_sim` — the firmware's ui/app/render/net
  layers compiled UNMODIFIED against LVGL 9 + SDL2 in a 1024×600 window;
  `sim/shim/` fakes ESP-IDF (pthread FreeRTOS, POSIX esp_http_client,
  file-backed NVS, /storage → `sim/simfs/`), `sim/stub/` fakes WiFi/mDNS.
  Tables via `DWT_SIM_TABLES="Name=url,..."` (default DWSIM=127.0.0.1:8080 —
  run the table sim). Device-specific bugs (internal-RAM exhaustion, stack
  sizes, flash-cache faults) do NOT reproduce here — the sim is for UI/protocol
  work, hardware still gates release.

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
