// Desktop stub for net/discovery.h. Tables come from DWT_SIM_TABLES
// ("Name=http://host:port,Name2=...") — default: the table sim on localhost.
#include "net/discovery.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

esp_err_t discovery_init(void)
{
    return ESP_OK;
}

int discovery_scan(table_info_t *out, int max, uint32_t timeout_ms)
{
    (void)timeout_ms;
    usleep(300 * 1000);  // pretend to browse

    const char *env = getenv("DWT_SIM_TABLES");
    char spec[512];
    snprintf(spec, sizeof(spec), "%s",
             env != NULL ? env : "DWSIM=http://127.0.0.1:8080");

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(spec, ",", &save); tok != NULL && n < max;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        snprintf(out[n].name, sizeof(out[n].name), "%s", tok);
        snprintf(out[n].url, sizeof(out[n].url), "%s", eq + 1);
        n++;
    }
    return n;
}
