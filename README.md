# Dune Weaver Touch (ESP32-S3)

A standalone touch panel for Dune Weaver sand tables, running on the Waveshare
ESP32-S3-Touch-LCD family. It is a
firmware reimplementation of the PySide6/QML kiosk app in
[`dune-weaver-pi/dune-weaver-touch`](https://github.com/tuanchris/dune-weaver-pi),
talking directly to the table's FluidNC firmware over its stateless HTTP/JSON
API (WiFi + mDNS discovery — no Raspberry Pi in between).

## Hardware

**Recommended: the ESP32-S3-Touch-LCD-5** (5", 800×480) — *not* the 5**B**.

The boards are one design with different panels, and the panel decides
everything that matters. The 5B's 1024×600 needs a 21 MHz pixel clock, which is
~24 Hz refresh; a frame-inverted panel beats at half that, putting an 11–13 Hz
flicker right at peak eye sensitivity. It is invisible at white and black and
plainly visible in mid-greys, so the 5B has to run the **light** theme. The
21 MHz is already the ceiling — 32 and 40 MHz were tried on hardware and both
tore. The 800×480 boards run 16 MHz for ~39 Hz, beat near 19.5 Hz, and do not
flicker (measured), so they get the dark theme the UI was designed around. They
also match the reference app's own 800×480 geometry exactly.

| | 5 *(recommended)* | 5B | 7 |
|---|---|---|---|
| Panel | 5" 800×480 | 5" 1024×600 | 7" 800×480 |
| PCLK / refresh | 16 MHz / ~39 Hz | 21 MHz / ~24 Hz | 16 MHz / ~39 Hz |
| Flicker | none | **mid-greys** | none |
| Default theme | dark | light | dark |
| Build env | `waveshare-5` | `waveshare-5b` | `waveshare-7` |
| Status | untested | validated | validated |

Common to all three: ESP32-S3-WROOM-1-N16R8 (16 MB quad flash, 8 MB octal
PSRAM), GT911 capacitive touch (I2C, 5-point), CH422G IO expander (LCD/touch
reset, backlight, SD CS), TF slot, RS485, CAN, battery charger. The GPIO map is
identical across them — only resolution and RGB timings differ.

The 7's pixels are 7.6% wider than tall (154.08 × 85.92 mm over 800×480), so
its build corrects circles in the UI. The 5's are square, so it does not.

**A note on TF cards:** use a plain 8–16 GB non-UHS card. SPI mode has been
optional in the SD spec since 2.0 and this board has no alternative (chip select
is on the CH422G), so some larger, faster cards fail the high-speed switch and
will not mount at all.

Note: UART0 (GPIO43/44) drives the RS485 transceiver, so the serial console is
the **native USB port** (`/dev/cu.usbmodem*`).

## Stack

- PlatformIO (`platform = espressif32@6.12.0`, `framework = espidf`), with
  ESP-IDF pinned to **5.5.2** — 5.5.0 misses the fix for
  [esp-idf#14000](https://github.com/espressif/esp-idf/issues/14000), where the
  TF card stops mounting after any reboot that does not power-cycle it
- LVGL 9 via managed components, with `esp_lvgl_port` driving the RGB panel
  (double framebuffer in PSRAM, bounce buffers, tearing avoidance)
- `esp_lcd_touch_gt911` for touch, `mdns` for table discovery

## Build / flash / monitor

```bash
pio run                 # build
pio run -t upload       # flash over USB
pio device monitor      # serial console (USB CDC)
```

If the port doesn't enumerate, hold BOOT while plugging in, then release.

## Status

Networked and live on hardware — all five pages walked against a real table:

- Hardware bring-up: RGB panel + GT911 touch + LVGL 9 (verified on the board)
- WiFi provisioning from the Control page (scan, on-screen keyboard, join),
  credentials in NVS; SNTP on first connect
- mDNS table discovery (`_http._tcp` filtered to `model=dune-weaver`),
  auto-connect on boot, manual address entry, table password (`X-Sand-Key`)
- Full firmware HTTP client: status polling with adaptive backoff, patterns
  (ETag-aware), playlists (create/edit/delete/run with mode/shuffle/rest/clear),
  run pattern with clear modes, transport (pause/resume/stop/skip on a fast
  worker lane), speed, LED effects/palettes/colors + ball tracker, homing,
  goto, restart, auto-play (`$Playlist/Autostart`), clock sync
- Pattern previews off a TF card: `tools/make_pattern_sd.py` renders each `.thr`
  to a 300×300 4-bit alpha mask, and the panel composites it with the theme's
  colours through a RAM LRU. The panel itself never renders or stores previews —
  no card just means placeholder dishes (see `docs/PORTING_NOTES.md` §6/§7a)
- Outfit + Material Icons Round fonts, screen sleep timeout, and a UI simulator
  (`sim/`) that builds the ui/app/render/net layers unmodified against LVGL 9 +
  SDL2, so UI and protocol work needs no board

Deferred: live night/day retheme (applies after restart), and a playlist
manifest route on the table so a playlist list costs one request instead of
`1 + N`. `docs/PORTING_NOTES.md` is the protocol/UX contract; `STATE.md` has
the current validation state and backlog.

## License

Dune Weaver Touch is available under a **dual license**, the same terms as the
rest of Dune Weaver:

### Open Source License (GPL-3.0)

For open-source projects and personal use, this firmware is licensed under the
[GNU General Public License v3.0](LICENSE-GPL-3.0).

You are free to use, modify, and distribute this software under GPL-3.0 terms,
provided that derivative works are also licensed under GPL-3.0 and source code
is made available.

### Commercial License

For commercial use, proprietary applications, OEM/embedded deployments, or if
you cannot comply with GPL-3.0 requirements, a commercial license is available.

Contact: hello@duneweaver.com

See the [LICENSE](LICENSE) file for full details.
