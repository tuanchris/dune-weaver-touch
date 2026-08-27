// Firmware update over HTTP, the same contract dune-weaver-firmware serves at
// /updatefw: a client PUSHES the image to this panel rather than the panel
// pulling one. That is what keeps TLS off the device -- the cert bundle plus a
// 16 KB mbedTLS session would come out of internal RAM, this project's scarcest
// resource (see CLAUDE.md on the WiFi beacon storm).
//
//   GET  /updatefw   -> probe. 200 {"status":"ready"} / 409 {"status":"busy"}
//   POST /updatefw   -> multipart/form-data. Field "<name>S" carries the image
//                       size in bytes, field "<name>" the app image itself.
//                       200 {"status":"ok"} then reboot ~1 s later; 500
//                       {"status":"failed"} and the running image is untouched.
//
// Every response body is {"status", "code", "fw", "mcu"}, where "code" is the
// raw upload-status enum (0 none, 1 failed, 2 cancelled, 3 successful,
// 4 ongoing) and "mcu" is the build target, so a client picks the matching
// image before streaming megabytes -- the ESP32 and S3 share the 0xE9 image
// magic, so a wrong-target image is only rejected at the very end.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Starts the HTTP server. Safe to call before WiFi is up: the listener binds
// INADDR_ANY, so it starts serving when the interface gets an address and
// survives reconnects without re-binding.
esp_err_t ota_init(void);

// The version baked into the app descriptor by the IDF build from version.txt
// (e.g. "0.1.0"). Never NULL.
const char *ota_version(void);

// Confirms the running image so the bootloader stops holding a rollback. Call
// once the app has reached a state you would ship. A no-op unless this boot is
// the first after an update.
void ota_mark_valid(void);

// -1 when idle, else 0..100. Written from the HTTP task, read by the UI.
int ota_progress_pct(void);

// --- Pulling an update (the Control page's Update button) ---
//
// The push path above cannot be self-triggered, so the panel also PULLS: it
// asks GitHub for the latest release tag, then streams
// releases/<tag>/firmware.bin off the default branch -- the same artifacts the
// web installer flashes, which the release workflow commits for exactly this
// reason (release-asset URLs redirect to a host that would need another TLS
// session and sends no CORS header).
//
// This is the one place the panel speaks HTTPS. It costs an mbedTLS session out
// of internal RAM, so it runs only on demand or once per connect, never in a
// poll loop.
typedef enum {
    OTA_IDLE = 0,
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_AVAILABLE,
    OTA_DOWNLOADING,
    OTA_INSTALLED,  // reboot scheduled
    OTA_ERROR,
} ota_state_t;

ota_state_t ota_state(void);

// Tag of the newest release, "" until a check has succeeded.
const char *ota_latest_version(void);

// Human-readable reason for OTA_ERROR, "" otherwise.
const char *ota_error(void);

// Both queue onto the jobs worker and return immediately — safe from an LVGL
// event callback. ota_update_async() is a no-op unless a check found something.
void ota_check_async(void);
void ota_update_async(void);

// Fired on every state/progress change, from the jobs task. The callback must
// take lvgl_port_lock itself if it touches LVGL.
typedef void (*ota_state_cb_t)(void);
void ota_set_state_cb(ota_state_cb_t cb);
