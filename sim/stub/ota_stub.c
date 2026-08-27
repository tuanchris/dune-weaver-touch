// Desktop stub for net/ota.h. The real module needs esp_http_server,
// esp_https_ota and a flash partition to write, none of which exist here — but
// the Control page's FIRMWARE card and the nav badge are ordinary UI and must
// stay verifiable without a board.
//
//   DWT_SIM_OTA=v0.9.9   a check finds that version (badge + "Update now")
//   DWT_SIM_OTA=none     a check finds nothing (default; "Up to date")
//   DWT_SIM_OTA=fail     a check errors
//
// "Downloading" runs on a timer so the progress text and the disabled button
// are exercised; it stops at OTA_INSTALLED rather than pretending to reboot.
#include "net/ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "app/jobs.h"

static ota_state_t s_state = OTA_IDLE;
static char s_latest[32];
static char s_err[96];
static ota_state_cb_t s_cb;
static int s_pct = -1;

const char *ota_version(void)
{
    return "0.1.0";
}

int ota_progress_pct(void)
{
    return s_pct;
}

ota_state_t ota_state(void)
{
    return s_state;
}

const char *ota_latest_version(void)
{
    return s_latest;
}

const char *ota_error(void)
{
    return s_err;
}

void ota_set_state_cb(ota_state_cb_t cb)
{
    s_cb = cb;
}

esp_err_t ota_init(void)
{
    return ESP_OK;
}

void ota_mark_valid(void) {}

static void emit(ota_state_t st)
{
    s_state = st;
    if (s_cb != NULL) {
        s_cb();
    }
}

static void check_job(void *arg)
{
    (void)arg;
    const char *env = getenv("DWT_SIM_OTA");
    if (env != NULL && strcmp(env, "fail") == 0) {
        snprintf(s_err, sizeof(s_err), "Could not reach GitHub");
        emit(OTA_ERROR);
        return;
    }
    if (env == NULL || strcmp(env, "none") == 0) {
        emit(OTA_UP_TO_DATE);
        return;
    }
    snprintf(s_latest, sizeof(s_latest), "%s", env);
    emit(OTA_AVAILABLE);
}

void ota_check_async(void)
{
    emit(OTA_CHECKING);
    jobs_submit(check_job, NULL);
}

// Runs on the jobs worker (the shim's esp_timer has no create/periodic, only
// esp_timer_get_time), so the sleep blocks nothing the UI needs.
static void download_job(void *arg)
{
    (void)arg;
    for (s_pct = 0; s_pct < 100; s_pct += 5) {
        if (s_cb != NULL) {
            s_cb();
        }
        usleep(150 * 1000);
    }
    s_pct = -1;
    emit(OTA_INSTALLED);
}

void ota_update_async(void)
{
    if (s_state != OTA_AVAILABLE) {
        return;
    }
    s_pct = 0;
    emit(OTA_DOWNLOADING);
    jobs_submit(download_job, NULL);
}
