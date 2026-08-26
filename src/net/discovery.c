// mDNS discovery of Dune Weaver tables (contract: discovery.h). Mirrors the
// reference app's discovery.py: browse _http._tcp, keep only services whose
// TXT record says model=dune-weaver, and build the base URL from the numeric
// IPv4 address. Some tables have underscores in their hostnames, which breaks
// .local resolution on several stacks — the IP-based URL sidesteps hostname
// resolution entirely. IPv4 only: boards publish no AAAA and dual-stack
// lookups stall.
#include "discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "mdns.h"

static const char *TAG = "discovery";

#define DISCOVERY_MAX_RESULTS 20
#define DISCOVERY_DEFAULT_TIMEOUT_MS 3000

static bool s_mdns_ready;

esp_err_t discovery_init(void)
{
    if (s_mdns_ready) {
        return ESP_OK;
    }
    // mdns_init() is itself idempotent (returns ESP_OK when the server is
    // already running), so an init elsewhere in the app is harmless.
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    s_mdns_ready = true;
    return ESP_OK;
}

// TXT filter: keep only advertisements that declare themselves a sand table
// (model=dune-weaver; other keys like api/ws are ignored here).
static bool txt_is_dune_weaver(const mdns_result_t *r)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        const mdns_txt_item_t *item = &r->txt[i];
        if (item->key && strcmp(item->key, "model") == 0) {
            return item->value && strcmp(item->value, "dune-weaver") == 0;
        }
    }
    return false;
}

// First IPv4 in the result's address list, or NULL if it only has IPv6.
static const esp_ip4_addr_t *first_ipv4(const mdns_result_t *r)
{
    for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            return &a->addr.u_addr.ip4;
        }
    }
    return NULL;
}

// Display name = instance name up to the first '.'
// ("Dune Weaver._http._tcp.local" → "Dune Weaver").
static void copy_display_name(char *dst, size_t dst_size, const mdns_result_t *r, const char *fallback)
{
    const char *src = r->instance_name;
    if (!src || !src[0]) {
        src = r->hostname;  // some responders omit the instance label
    }
    if (!src || !src[0]) {
        strlcpy(dst, fallback, dst_size);
        return;
    }
    size_t n = strcspn(src, ".");
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int discovery_scan(table_info_t *out, int max, uint32_t timeout_ms)
{
    if (out == NULL || max <= 0) {
        return -1;
    }
    if (discovery_init() != ESP_OK) {
        return -1;
    }
    if (timeout_ms == 0) {
        timeout_ms = DISCOVERY_DEFAULT_TIMEOUT_MS;
    }

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_http", "_tcp", timeout_ms, DISCOVERY_MAX_RESULTS, &results);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns query failed: %s", esp_err_to_name(err));
        return -1;
    }

    int count = 0;
    for (const mdns_result_t *r = results; r != NULL && count < max; r = r->next) {
        if (!txt_is_dune_weaver(r)) {
            continue;
        }
        const esp_ip4_addr_t *ip4 = first_ipv4(r);
        if (ip4 == NULL) {
            ESP_LOGD(TAG, "skipping %s: no IPv4 address",
                     r->instance_name ? r->instance_name : "(unnamed)");
            continue;
        }

        char ip_str[16];  // "255.255.255.255" + NUL
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(ip4));

        table_info_t entry = {0};
        if (r->port == 0 || r->port == 80) {
            snprintf(entry.url, sizeof(entry.url), "http://%s", ip_str);
        } else {
            snprintf(entry.url, sizeof(entry.url), "http://%s:%u", ip_str, (unsigned)r->port);
        }
        copy_display_name(entry.name, sizeof(entry.name), r, ip_str);

        bool duplicate = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(out[i].url, entry.url) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        out[count++] = entry;
        ESP_LOGI(TAG, "discovered table: %s @ %s", entry.name, entry.url);
    }

    mdns_query_results_free(results);
    return count;
}
