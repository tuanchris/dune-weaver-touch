// mDNS discovery of Dune Weaver tables: browse _http._tcp.local. and keep
// only services whose TXT record has model=dune-weaver. Prefer numeric IPv4
// (boards publish no AAAA; .local resolution is unreliable on some networks).
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char name[64];  // service instance name up to the first '.'
    char url[128];  // "http://<ipv4>[:port]", port omitted when 80
} table_info_t;

// Idempotent; requires WiFi up. Safe to call before every scan.
esp_err_t discovery_init(void);

// Blocking browse (call from the jobs task). Returns count (deduped by url),
// or <0 on error. timeout_ms ~3000 matches the reference app.
int discovery_scan(table_info_t *out, int max, uint32_t timeout_ms);
