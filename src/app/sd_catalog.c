#include "sd_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "sd_catalog";

#define SD_MANIFEST "/sdcard/patterns.json"
// A manifest bigger than this is corrupt, not a catalog (10k patterns ≈ 400 KB).
#define SD_MANIFEST_MAX (1024 * 1024)

// Weak default so the desktop sim (no card driver) links; the firmware's
// board/sdcard.c provides the real remount. Lets a card inserted after boot
// be picked up by the next Browse refresh.
__attribute__((weak)) esp_err_t sdcard_remount(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool sd_catalog_present(void)
{
    struct stat st;
    return stat(SD_MANIFEST, &st) == 0 && st.st_size > 0;
}

// Mirror fw_client's list normalization: entries relative to /patterns.
static const char *normalize_entry(const char *s)
{
    if (strncmp(s, "/sd/patterns/", 13) == 0) {
        return s + 13;
    }
    if (strncmp(s, "/patterns/", 10) == 0) {
        return s + 10;
    }
    while (*s == '/') {
        s++;
    }
    return s;
}

esp_err_t sd_catalog_get(fw_str_list_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->items = NULL;
    out->count = 0;

    if (!sd_catalog_present()) {
        sdcard_remount();  // card may have been inserted after boot
    }
    FILE *f = fopen(SD_MANIFEST, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    struct stat st;
    if (stat(SD_MANIFEST, &st) != 0 || st.st_size <= 0 || st.st_size > SD_MANIFEST_MAX) {
        fclose(f);
        ESP_LOGW(TAG, "manifest missing/oversized (%ld B)", (long)(st.st_size));
        return ESP_ERR_INVALID_SIZE;
    }

    char *text = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (text == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(text, 1, (size_t)st.st_size, f);
    fclose(f);
    text[rd] = '\0';

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "manifest is not a JSON array");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Allocations mirror fw_client's str_list_dup (SPIRAM), so
    // fw_str_list_free works on either list and internal RAM stays untouched.
    int n = cJSON_GetArraySize(root);
    char **items = heap_caps_malloc((size_t)(n > 0 ? n : 1) * sizeof(char *),
                                    MALLOC_CAP_SPIRAM);
    if (items == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    int count = 0;
    cJSON *entry;
    cJSON_ArrayForEach(entry, root) {
        if (!cJSON_IsString(entry) || entry->valuestring == NULL) {
            continue;
        }
        const char *rel = normalize_entry(entry->valuestring);
        if (rel[0] == '\0') {
            continue;
        }
        size_t len = strlen(rel) + 1;
        char *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (copy == NULL) {
            break;  // keep what we have
        }
        memcpy(copy, rel, len);
        items[count++] = copy;
    }
    cJSON_Delete(root);

    if (count == 0) {
        free(items);
        ESP_LOGW(TAG, "manifest parsed but empty");
        return ESP_ERR_NOT_FOUND;
    }
    out->items = items;
    out->count = count;
    return ESP_OK;
}
