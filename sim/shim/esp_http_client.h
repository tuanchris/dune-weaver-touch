// Desktop shim: esp_http_client over plain POSIX sockets. Implements exactly
// the manual open/fetch_headers/read flow fw_client.c drives, including the
// ON_HEADER event (ETag capture) and -ESP_ERR_HTTP_EAGAIN timeout signaling.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_ERR_HTTP_BASE 0x7800  // distinct from fw_client's FW_ERR_* 0x7001/2
#define ESP_ERR_HTTP_EAGAIN (ESP_ERR_HTTP_BASE + 7)

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
} esp_http_client_event_id_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    void *user_data;
    char *header_key;
    char *header_value;
    char *data;
    int data_len;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
} esp_http_client_method_t;

typedef struct {
    const char *url;
    int timeout_ms;
    http_event_handle_cb event_handler;
    void *user_data;
    bool disable_auto_redirect;
    int buffer_size;
    int buffer_size_tx;
} esp_http_client_config_t;

typedef struct sim_http *esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c,
                                     esp_http_client_method_t method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,
                                     const char *key, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len);
int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
