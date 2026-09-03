# dune-weaver-touch (ESP32-S3)

LVGL firmware for the Waveshare ESP32-S3-Touch-LCD boards that reimplements the
PySide6/QML touch app at `/Volumes/SSD/projects/dune-weaver-pi/dune-weaver-touch`.
Two panel variants build from this tree: the **5B** (5", 1024x600, default) and
the **7** (7", 800x480, `-DBOARD_WAVESHARE_7`). They are the same base design —
identical GPIO map and CH422G sequences — so the split is nine `#define`s in
`board.h`, not a second board file. See "Board variants" below.
Read `STATE.md` first for the current status, validation state, and backlog.
That app is the reference for every feature, label, and behavior; `docs/PORTING_NOTES.md`
holds the distilled contract (firmware HTTP API, page specs, constants). When in
doubt, read the QML source — do not invent behavior.

## Commands

- **Four Waveshare envs** (the Elecrow boards, `crowpanel-adv-5` and
  `crowpanel-7`, have their own sections below). `waveshare-5b` (1024x600, and it carries
  `-DUI_DEBUG_RGB_STOP` by default — standing practice per Tuan 2026-08-28, the
  live white-halo experiment, so every bench flash of the 5B gets it);
  `waveshare-5b-release` (the same board WITHOUT that flag — what
  `tools/build_release.py` ships, because the prototype is unmeasured and must
  not reach real panels); `waveshare-5` and `waveshare-7` (both 800x480).
  Drop the flag and the extra env together once the halo question is settled.
- **Two independent build axes.** `BOARD_PANEL_800X480` is geometry —
  resolution, RGB timings, theme tokens, the Browse grid, the dark default.
  `BOARD_WAVESHARE_7` says ONLY that the glass has non-square pixels. The 5 and
  the 7 share the first; only the 7 sets the second. Do not re-merge them: a 5"
  800x480 panel is ~0.135 mm square pixels and would be visibly over-corrected.
- **Releases carry two images.** `firmware.bin` is the 5B (unchanged, so panels
  in the field keep updating) and `firmware-800x480.bin` is the 5/7; `ota.c`
  picks by `BOARD_PANEL_800X480`, and the manifest offers both as a `Board`
  choice so the web installer can ask. Both boards are esp32s3 and nothing else
  distinguishes them, so a single image would let an 800x480 panel flash a
  1024x600 build. A missing image 404s, which fails safe — a manifest that
  omits one does NOT, which is how v0.1.3 shipped with the 800x480 image
  present and uninstallable.
- **`tools/release_spec.py` declares what a release is** (envs, images,
  offsets, the `Board` → `Installation type` tree); `build_release.py` builds
  against it and `tools/check_release.py` verifies against it. Add a panel
  there, in `BOARDS`, and the check fails until the manifest offers it. Never
  hard-code an offset or an image name anywhere else, and never publish past
  a `check_release` failure — it runs in `build_release.py`, in the release
  workflow after the artifacts are committed, and in `check-releases.yml` on
  every push touching `releases/`. See RELEASING.md.
- Build: `pio run -e waveshare-5b` (or `-e waveshare-7`) · Flash: add `-t upload`
  · Monitor: `pio device monitor -e <env>`. Bare `pio run` builds BOTH envs.
- OTA (same contract as dune-weaver-firmware, `src/net/ota.c`): the panel RUNS
  an HTTP server and takes a PUSHED image — it never pulls, which is what keeps
  TLS and its internal-RAM cost off the device.
  `curl http://<panel>/updatefw` probes (`ready`/`busy`);
  `curl -F "firmware.binS=$(stat -f%z fw.bin)" -F "firmware.bin=@fw.bin" http://<panel>/updatefw`
  flashes the inactive slot and reboots ~1 s after answering `{"status":"ok"}`.
  Rollback is armed: `ota_mark_valid()` at the end of `app_main` is what stops
  the bootloader reverting on the next reset, so anything that panics during
  init undoes itself. **Crossing onto the OTA partition table needs one USB
  flash and re-provisioning** — see partitions.csv.
- The panel ALSO pulls, for Control's Update button: the GitHub API for the
  latest tag, then `releases/<tag>/firmware.bin` off `main` — the same
  artifacts the web installer flashes, which is why the release workflow
  commits them. This is the only HTTPS the panel speaks and a TLS session comes
  out of internal RAM, so it runs once per connect and on demand, never in a
  poll loop. Control promotes the FIRMWARE card to the top (and lights a dot on
  the Control tab) while an update is waiting or installing; `control_reorder()`
  owns that and the offline WIFI promotion together, so they cannot fight over
  the top slot.
- Console is native USB CDC (`/dev/cu.usbmodem*`); UART0 is physically wired to RS485.
- First build downloads managed components into `managed_components/` (gitignored).
  **No `dependencies.lock` is tracked**, so every fresh machine pulls the newest
  LVGL and esp_lvgl_port (9.5.0 / 2.9.0 on 2026-09-03). Behaviour validated on
  an older LVGL can differ: 9.5 pauses the refresh timer when nothing is
  invalid, which stalled the sleep-wake gate until it provoked its own frames.
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
  run the table sim); `DWT_SIM_WIFI=off` starts disconnected, which is the only
  way to reach the offline layouts here (Control puts WIFI first when the panel
  has no network) — a join from the Scan list still succeeds, so one run covers
  both orders. `DWT_SIM_OTA=v0.9.9|none|fail` fakes the update check
  (`stub/ota_stub.c`), which is the only way to see the nav dot, the FIRMWARE
  card's promoted position and the download progress without cutting a release. Device-specific bugs (internal-RAM exhaustion, stack
  sizes, flash-cache faults) do NOT reproduce here — the sim is for UI/protocol
  work, hardware still gates release.
- Preview pipeline check: `DWT_SIM_PREVIEW_SELFTEST=<pattern.thr>
  ./sim/build/dwt_sim` dumps that pattern's tile at every size the UI asks for
  and exits; `python sim/check_tiles.py out.png` renders the dumps. Use it on
  any card-format change — the 300 px path (Now Playing, Browse detail) is
  otherwise only reachable by tapping, so it silently goes unverified.

## Board variants

- `pio run -e waveshare-5b` (1024x600) and `-e waveshare-7` (800x480). The
  variant is one flag, `-DBOARD_WAVESHARE_7`, consumed only by `board.h`; the
  sim takes the same switch as `cmake -S sim -B sim/build -DDWT_BOARD=7`.
- **Everything except resolution and RGB timings is identical** — RGB bus
  GPIOs, I2C 8/9, TP_IRQ 4, GT911, CH422G addresses and EXIO sequences, 16MB
  quad flash, 8MB octal PSRAM. Verified against Waveshare's own demos
  (`08_lvgl_Porting` for the 5B, `09_lvgl_v9_demo` for the 7). Do not
  re-derive the pin map per board; there is only one.
- The 7 runs 800x480 @ 16 MHz with 8/8/4 porches both axes: 820 x 500 =
  410,000 clocks/frame, so **~39 Hz**, versus the 5B's 24 Hz. Its
  frame-inversion beat is ~19.5 Hz, clear of the 8-15 Hz band that forces the
  light theme on the 5B. **Confirmed on hardware 2026-08-29: the 7 does not
  flicker.** So the flicker constraint is a 5B property, not a family one —
  `settings.c` defaults the 7 to DARK and the 5B to light, and the theme
  default is a panel fact rather than a preference. Do not "unify" them.
- **Do not leave a serial reader attached to the native USB port.** On
  USB-Serial-JTAG the download-boot request travels over USB itself, so
  macOS's DTR/RTS pattern when a host OPENS the CDC port resets the chip into
  `DOWNLOAD(USB/UART0)`, where it parks at `waiting for download` and nothing
  drives the panel. Symptom: a black screen and a boot log that only ever
  shows the ROM banner. Proof it is the opening and not a stale buffer: each
  open prints a different `Saved PC`. `pio device monitor` handles the
  sequence properly and is the right tool; an ad-hoc pyserial listener is not,
  and a reconnecting one keeps the board down for as long as it runs. To check
  whether the app is actually alive, detach everything and look at the glass.
- **The 7 has two USB-C ports and they behave differently.** The port
  labelled `USB` is the S3's native USB (GPIO19/20, `303a:1001`); the one next
  to the UART-select slide switch is a CH343 bridge (`1a86:55d3`) that the
  wiki calls UART1. BOTH have flashed this board successfully (2026-08-28).
  What actually bites is that the CH343 port's auto-reset can fail to enter
  download mode at all — esptool then reports `No serial data received` on
  every `--before` strategy and an EN pulse yields a zero-byte boot log, which
  reads exactly like a dead board. Hold BOOT while plugging in to force the
  ROM, or use the native-USB port, which has its own ROM download path.
  Do not conclude from that failure that the UART port cannot flash; it can.
- On the 7, **CH422G EXIO5 muxes GPIO19/20 between USB and the CAN
  transceiver** (wiki: low = USB, high = CAN). `EXIO_RUN_DEFAULT` (0x1A)
  leaves it low, which is what keeps the native-USB console alive — do not
  set bit 5 in the run mask. Waveshare's own 7 demo writes 0x2C/0x2E, i.e.
  bit 5 high, which is the likely reason a board running their factory
  firmware never enumerates until BOOT is held. `EXIO_TP_RESET_LOW/HIGH`
  (0x28/0x2A) still blip bit 5 high during the boot touch-reset; too brief to
  drop enumeration in practice, but worth clearing when that code is touched.
- **The UI scales per panel.** `theme.h` holds two token sets: the 5B's are
  the QML values x1.5 (237 PPI), the 7's are the QML values verbatim, because
  800x480 @ ~133 PPI IS the QML app's target — so read the reference app for
  the 7's numbers rather than dividing the 5B's by 1.5. `headerHeight` 60 and
  `navHeight` 64 were never scaled and are shared. Fonts come in both scales
  (18/21/26/36 and 12/14/17/24); `src/CMakeLists.txt` lists all eight and
  `--gc-sections` drops the unused set (measured: the 5B image moved 44 bytes,
  not 250 KB).
- For one-off sizes the token scale does not cover, write the QML pixel value
  and wrap it in **`TH_S()`** (theme.h) rather than hardcoding. Integer math
  reproduces every 5B literal exactly — `TH_S(56)`=84, `TH_S(300)`=450,
  `TH_S(380)`=570, `TH_S(427)`=640, `TH_S(227)`=340 — so the 5B is bit-stable
  while the 7 gets the reference value.
- Browse re-derives its grid per panel (`page_browse.c`): the 7 is 4x2 at
  CARD_W 144 / CARD_H 163 / preview 136, and **height is its binding axis**
  where the 5B's was width. Detail preview is 300 there, which lands exactly
  on the master tile size, so it blits 1:1.

## Elecrow CrowPanel Advance 5.0-HMI (env `crowpanel-adv-5`)

A third board, and NOT a Waveshare derivative: `src/board/board_crowpanel5.c`
replaces `board.c` wholesale for this env. 5" 800x480, ESP32-S3-WROOM-1-N16R8,
so the whole UI carries over via `BOARD_PANEL_800X480`; pixels are square, so
it does NOT set `BOARD_WAVESHARE_7`. Pin map from Elecrow's factory code
(`Elecrow-RD/CrowPanel-Advance-HMI-ESP32-AI-Display`, `5.0/factory_code`).

- **The DIP table changed between board revisions, and the silkscreen, the
  schematic and the factory code all show the OLD one.** TF Card is
  `S1 S0 = 1 0` on v1.0 but **`1 1` ("MIC & TF Card") on v1.1+**, which is what
  ships now. ON really does mean 1 (K1's commons go to 3V3, R56/R57 are
  pull-downs). Wrong position = `sdmmc_card_init failed (0x107)` /
  `ESP_ERR_TIMEOUT` forever, indistinguishable from a dead card. Verified
  2026-09-01 by retrying the mount while cycling all four positions -- do that
  rather than trusting any printed table.
- Which revision you have shows in the boot log: `expander probe: TCA9534@0x18
  ... STC8@0x30 ...`. v1.0 has the TCA9534/PCA9557; v1.1+ has the STC8.
- **PCLK is 16 MHz, not the 21 Elecrow's LovyanGFX demo uses.** At 21 the image
  DRIFTS (RGB DMA underrun). These timings leave 20 clocks of horizontal
  blanking against the 5B's 345, so there is almost no slack to refill bounce
  buffers in; LovyanGFX gets away with it by driving the panel differently.
  Buy refresh back with PORCHES, never with PCLK.
- **Backlight is an STC8H1K28 at I2C 0x30 and the byte is inverted between
  revisions**: v1.1 is `0x05`(off)..`0x10`(max), **v1.2+ is `0`(brightest)..
  `245`(off)**. Getting it backwards fails silently -- the screen just never
  goes dark on sleep. `STC8_BL_V11` in board_crowpanel5.c flips it. Because
  this is a real brightness control, sleep can DIM instead of cutting the
  converter (`board_backlight_level`), which is the structural answer to the
  5B's white-halo artifact.
- **No USB-Serial-JTAG**: GPIO19/20 are the I2S microphone, so the console goes
  out UART0 to an onboard CH340K. `custom_sdkconfig` in platformio.ini sets
  `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` for this env only (via
  `tools/sdkconfig_env.py` -- PlatformIO has no per-env sdkconfig defaults).
- **macOS needs WCH's CH34x driver** for that CH340K (`1a86:7522` is
  vendor-class, unlike the CH343 on the Waveshare 7 which is USB CDC and works
  out of the box). Install it from `WCHSoftGroup/ch34xser_macos`, approve the
  system extension, reboot.
- **Chrome steals the port.** A page holding WebSerial permission reconnects
  the moment the board enumerates and takes it exclusively -- esptool then says
  "port is busy", or macOS never creates `/dev/cu.*` at all. Check with
  `lsof /dev/cu.wchusbserial*`. Clear the site's saved permission in Chrome.
- SD chip select is not on any GPIO (`gpio_cs = GPIO_NUM_NC`), same as the
  Waveshare boards. SD shares GPIO 4/5/6 with the I2S speaker through a CH486F
  analog mux that no GPIO can read or set -- only the DIP moves it.

## Elecrow CrowPanel 7.0-HMI (env `crowpanel-7`)

The ORIGINAL CrowPanel 7" (DIS07070H; Elecrow wiki "ESP32 Display-7.0 inch
Intelligent Touch Screen"). NOT the Advance series and NOT the Waveshare 7,
and none of their pin maps apply. `src/board/board_crowpanel7.c` replaces
`board.c` for this env. 800x480 RGB on an ESP32-S3-WROOM-1-N4R8 -- **4 MB
flash**, 8 MB octal PSRAM. Pin map and timings are Elecrow's LovyanGFX config,
identical across their V1.0/V2.0/V3.0 examples. Brought up 2026-09-03: boots,
renders, touch works. UNMEASURED: drift at 16 MHz, dark-theme flicker, the TF
card, sleep dimming.

- **4 MB flash is the trap.** Any 16 MB env flashed onto it asserts in flash
  init -- `spi_flash: Detected size(4096k) smaller than the size in the
  binary image header(16384k)` -- and reboots every 0.6 s. The assert prints
  on the console the IMAGE was built for, so a Waveshare image shows nothing:
  the UART port just repeats the ROM banner, `rst:0xc (RTC_SW_CPU_RST)` with
  `Saved PC` decoding to `esp_restart_noos`. A boot loop faster than
  app_main's ~650 ms start with no app output is this, not a crash in init.
  The env carries `partitions-4mb.csv` (same nvs/otadata offsets, two 1920 KB
  OTA slots) and `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`; the app fills ~85% of a
  slot, so watch the image size on this board.
- Console is UART0 through the onboard CH340 -- `/dev/cu.usbserial-*` with
  macOS's built-in driver, unlike the Advance's CH340K. Always pass the port:
  `pio run -e crowpanel-7 -t upload --upload-port /dev/cu.usbserial-XXXX`,
  `pio device monitor --port /dev/cu.usbserial-XXXX -b 115200`. DTR/RTS
  auto-reset works every time.
- **GPIO19/20 are the touch I2C** -- the S3's USB pins. The env disables the
  USB-Serial-JTAG peripheral AND the secondary console
  (`CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n`, `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`);
  only then does IDF release the USB pad at startup (`clk.c`). A fresh
  sdkconfig defaults the secondary console to USB-Serial-JTAG, which `select`s
  the peripheral back on and keeps the pad on the bus.
- **Touch I2C runs at 100 kHz** (`BOARD_TOUCH_I2C_HZ`). At the Waveshare
  boards' 400 kHz every GT911 register read returned 0xCF -- product ID and
  config version alike -- and touch was dead while the address still ACKed.
  At 100 kHz it reads `0x39,0x31,0x31` ("911"), config 101, and works.
- No IO expander and no touch reset: INT and RST reach nothing on any
  revision (every Elecrow example passes -1 for both), so `display.c` probes
  0x5D then 0x14 (`BOARD_TOUCH_ADDR_PROBE`). Raw GT911 coordinates match the
  panel with no mirroring -- Elecrow's example inverts twice (TAMC
  `ROTATION_NORMAL`, then `map(800..0)`), a no-op.
- Backlight is GPIO2 into the boost enable, driven by LEDC (1 kHz, 10-bit), so
  `board_backlight_level` is a real dimmer here. Nothing calls it yet.
- TF card is SPI 11/13/12 with a REAL chip select on GPIO10
  (`BOARD_SD_GPIO_CS`, the only board with one). Untested with a card.
- 16 MHz PCLK on Elecrow's 40/48/40 + 1/31/13 porches: 928 x 525 = 487,200
  clocks/frame, ~32.8 Hz, beat ~16.4 Hz -- just above the 8-15 Hz flicker
  band. Elecrow's own 15 MHz sits at 30.8 Hz / 15.4 Hz. H blanking is 128
  clocks, seven times the Advance's, so there is room to go up if it beats,
  but measure for drift first.
- Same 7" 800x480 glass geometry as the Waveshare 7, so the env sets
  `BOARD_WAVESHARE_7` (theme.h aspect correction only).
- **Not in `tools/release_spec.py`.** It needs its own bootloader, partition
  table and app image, and the spec has no per-board partition table. `ota.c`
  names `firmware-crowpanel-7.bin`, which 404s (fails safe) until a release
  carries it.

## Hardware facts (verified against Waveshare wiki + demo, do not "fix")

- 1024×600 RGB565 parallel panel, PCLK 21 MHz, porches H 145/170/30, V 23/12/2,
  `pclk_active_neg=1`. Pin map lives in `src/board/board.h`.
- **The panel flickers below ~45% luminance and there is no firmware fix.**
  1369×637 = 872,053 clocks/frame, so 21 MHz is ~24 Hz, and a frame-inverted
  panel beats at half the refresh — an 11–13 Hz flicker, peak eye sensitivity.
  It is invisible where the V-T curve is flat and visible where it is steep.
  Measured on hardware 2026-08-27 with the band ladder below: **≤45% flickers,
  ≥55% is clean.** PCLK is the only lever and the S3 cannot pay for it — peak
  framebuffer DMA is set by PCLK alone, so trimming porches buys refresh while
  removing the blanking slack the bounce buffer refills in. 40 MHz (160/4) was
  unusable in seconds; 32 MHz (160/5) jumped about once a second; 32 MHz with
  bounce buffers doubled to 80 KB internal was no better, which proves the
  limit is sustained PSRAM bandwidth, not buffer slack. All reverted. **The
  consequence is the theme:** dark mode puts 9 of its 10 large fills at 8–22%
  (`bg` 7.7, `surface` 10.9, `card` 14.4, `preview_dish` 9.2) and the whole UI
  beats; light mode puts them at 78–94% and the panel is clean. Do not "fix"
  the flicker in the driver — pick colours above 55%.
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
  with `-DUI_DEBUG_TAB_CYCLE` (auto-cycles tabs every 4 s). For panel
  uniformity and the flicker band, `-DUI_DEBUG_FLAT_FIELD` puts a full-screen
  band ladder above everything with sleep disabled: page 0 is a grey ladder
  labelled in % luminance (100…8), page 1 is the live theme's own tokens by
  name, tap to swap. Stacked bands beat cycling full-screen fills — a band
  that flickers sits right against ones that do not, so the edges are obvious.
  This is also how to tell real image sticking from what an un-driven panel
  always looks like (backlight on, RGB timing stopped = a violet wash with
  every non-uniformity on show; that is normal, not damage).
  `-DUI_DEBUG_SLEEP_CYCLE` sleeps after 8 s, synthesizes the wake tap 5 s
  later and logs every counted frame (plus the backlight pad level on the
  CrowPanel 7.0) — a hands-free soak for the wake path on a bench nobody is
  tapping.
- CH422G IO expander has no register pointer — each function is an I2C address:
  write `0x01` to addr `0x24` (all-output mode), then a bitmask to addr `0x38`.
  EXIO1=TP_RST, EXIO2=DISP/backlight, EXIO3=LCD_RST, EXIO4=SD_CS. Backlight is
  on/off only (no PWM).
- **EXIO2 does not stop the panel, despite being named DISP.** Verified on
  `ESP32-S3-Touch-LCD-5-Sch.pdf`: it reaches exactly one pin, CTRL on U2
  (AP3032KTR-G1, the backlight boost), while the LCD's own DISP input —
  connector pin 31 — is tied to 3V3 through R30 (0R). The 4.3 *does* share
  that net between panel and backlight, which is where the internet's
  "backlight off depolarizes the panel" advice comes from; it does not apply
  here. Consequences: backlight off is a true off and safe, the panel keeps
  refreshing whatever is in the framebuffer the whole time it is dark, and a
  motionless sleep shield therefore drives one frame into the glass for hours
  (see `screen_sleep.c` — that is real and it is what the repolarize flip is
  for). PWM dimming would need CTRL cut from EXIO2 and wired to GPIO6, the
  only unallocated pin in Waveshare's own GPIO table and not broken out.
- **A card that will not mount is usually the HOST, not the card.** Two
  independent causes, both hit on 2026-08-28 with a 32 GB card that a second
  card did not reproduce:
  1. `cmd=52 ... command CRC error` -> `sdmmc_io_reset: unexpected return:
     0x109` -> `no TF card mounted (ESP_ERR_INVALID_CRC)`. An ESP-IDF bug
     (espressif/esp-idf#14000): the SD spec wants CRC OFF in SPI mode outside
     CMD0/CMD8, but IDF turns it ON during init (`sdmmc_send_cmd_crc_on_off`)
     and the card KEEPS that state across a reset, because a soft reset does
     not cut card power. Fixed upstream by tolerating the error; present in
     v5.3+/v5.4/v5.5.1/v5.5.2 but NOT 5.5.0. **Product impact: any reboot that
     does not power-cycle the card can lose it — the OTA reboot included.**
     platformio.ini pins framework-espidf 3.50502.0 (IDF 5.5.2) for this, with
     `tools/patch_idf_sdmmc.py` as a no-op safety net on older frameworks.
  2. `sdmmc_enable_hs_mode_and_check: send_csd returned 0x108` ->
     `sdmmc_card_init failed (0x108)`. The HIGH-SPEED switch itself. Some
     cards cannot do 40 MHz in SPI mode and fail the MOUNT, not just reads —
     so `sdcard.c` uses `SDMMC_FREQ_DEFAULT` (20 MHz), not
     `SDMMC_FREQ_HIGHSPEED`. Do not "optimise" it back without testing more
     than one card.
  What is NOT the cause: the filesystem, the partition map, and the card's
  contents. Init aborts before CMD0, so no sector is ever read — verified by
  reformatting to clean FAT32/MBR with 1232/1232 tiles resolving and getting a
  byte-identical failure. Do not reformat, and do not blame the card first.
  (Open question: at 20 MHz CMD52 answers "command not supported" rather than
  a CRC error, so cause 1 may itself be speed-induced. The IDF bump is kept
  regardless — the soft-reboot bug is real and independent.)
- **A populated Browse does not mean the card mounted.** The pattern LIST comes
  from the table over HTTP (`page_browse: loaded N patterns (table)`); only the
  preview tiles come off the card. Card missing = full list, every dish a
  placeholder. Check `TF card mounted at /sd: N MB` in the log, not the UI.
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
- **`LV_EVENT_REFR_READY` is per repaint, not per tick.** LVGL 9.5 pauses the
  refresh timer when nothing is invalid. Anything that counts frames (the
  wake gate in `screen_sleep.c`) must invalidate to get the next one, or it
  parks forever on a quiet panel — which is exactly a bench with no table.
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
