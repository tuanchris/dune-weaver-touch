# Dune Weaver Touch (ESP32-S3)

A standalone touch panel for Dune Weaver sand tables, running on the
**Waveshare ESP32-S3-Touch-LCD-5B** (5" capacitive touch, 1024×600). It is a
firmware reimplementation of the PySide6/QML kiosk app in
[`dune-weaver-pi/dune-weaver-touch`](https://github.com/tuanchris/dune-weaver-pi),
talking directly to the table's FluidNC firmware over its stateless HTTP/JSON
API (WiFi + mDNS discovery — no Raspberry Pi in between).

## Hardware

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-5B |
| MCU | ESP32-S3-WROOM-1-N16R8 (16 MB quad flash, 8 MB octal PSRAM) |
| Panel | 5" 1024×600 RGB565 parallel, PCLK 21 MHz |
| Touch | GT911 capacitive (I2C, 5-point) |
| IO expander | CH422G (LCD/touch reset, backlight, SD CS) |
| Also on board | PCF85063 RTC, TF slot, RS485, CAN, battery charger |

Note: UART0 (GPIO43/44) drives the RS485 transceiver, so the serial console is
the **native USB port** (`/dev/cu.usbmodem*`).

## Stack

- PlatformIO (`platform = espressif32@6.12.0`, `framework = espidf`, ESP-IDF 5.5)
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
