// Desktop shim implementations: logging clock, FreeRTOS-over-pthreads, the
// LVGL port lock, file-backed NVS, LittleFS-as-directory (+ path remap), and
// esp_http_client over POSIX sockets. Compiled WITHOUT sim_remap.h.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

// Where the fake flash lives: <repo>/sim/simfs (created on demand).
#ifndef SIM_FS_ROOT
#define SIM_FS_ROOT "simfs"
#endif

// ------------------------------------------------------------------- clock

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t sim_log_ms(void)
{
    return esp_timer_get_time() / 1000;
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    default: {
        static _Thread_local char buf[24];
        snprintf(buf, sizeof(buf), "0x%x", (unsigned)err);
        return buf;
    }
    }
}

// ------------------------------------------------------------ LVGL port lock

static pthread_mutex_t s_lvgl_mutex;
static pthread_once_t s_lvgl_once = PTHREAD_ONCE_INIT;

static void lvgl_mutex_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lvgl_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;  // device code always passes 0 = wait forever
    pthread_once(&s_lvgl_once, lvgl_mutex_init);
    pthread_mutex_lock(&s_lvgl_mutex);
    return true;
}

void lvgl_port_unlock(void)
{
    pthread_mutex_unlock(&s_lvgl_mutex);
}

// ------------------------------------------------------- FreeRTOS primitives

struct sim_sem {
    pthread_mutex_t mutex;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct sim_sem *s = calloc(1, sizeof(*s));
    if (s != NULL) {
        pthread_mutex_init(&s->mutex, NULL);
    }
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)ticks;  // device code only uses portMAX_DELAY
    return pthread_mutex_lock(&sem->mutex) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    return pthread_mutex_unlock(&sem->mutex) == 0 ? pdTRUE : pdFALSE;
}

struct sim_queue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    char *items;
    UBaseType_t length, item_size, count, head;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct sim_queue *q = calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    q->items = malloc((size_t)length * item_size);
    if (q->items == NULL) {
        free(q);
        return NULL;
    }
    q->length = length;
    q->item_size = item_size;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void vQueueDelete(QueueHandle_t q)
{
    free(q->items);
    free(q);
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)ticks;  // device code sends non-blocking (0)
    pthread_mutex_lock(&q->mutex);
    if (q->count == q->length) {
        pthread_mutex_unlock(&q->mutex);
        return pdFALSE;
    }
    UBaseType_t tail = (q->head + q->count) % q->length;
    memcpy(q->items + (size_t)tail * q->item_size, item, q->item_size);
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    (void)ticks;  // device code receives with portMAX_DELAY
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }
    memcpy(item, q->items + (size_t)q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->length;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return pdTRUE;
}

struct sim_task {
    pthread_t thread;
    TaskFunction_t fn;
    void *arg;
    // One-slot notification (ulTaskNotifyTake / xTaskNotifyGive).
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t notify_count;
};

static pthread_key_t s_task_key;
static pthread_once_t s_task_key_once = PTHREAD_ONCE_INIT;

static void task_key_init(void)
{
    pthread_key_create(&s_task_key, NULL);
}

static void *task_trampoline(void *arg)
{
    struct sim_task *t = arg;
    pthread_once(&s_task_key_once, task_key_init);
    pthread_setspecific(s_task_key, t);
    t->fn(t->arg);
    return NULL;  // device tasks never return
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       uint32_t stack_bytes, void *arg, UBaseType_t prio,
                       TaskHandle_t *out_handle)
{
    (void)name;
    (void)stack_bytes;
    (void)prio;
    struct sim_task *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return pdFAIL;
    }
    t->fn = fn;
    t->arg = arg;
    pthread_mutex_init(&t->mutex, NULL);
    pthread_cond_init(&t->cond, NULL);
    if (out_handle != NULL) {
        *out_handle = t;  // set before the thread runs (state.c relies on it)
    }
    if (pthread_create(&t->thread, NULL, task_trampoline, t) != 0) {
        free(t);
        if (out_handle != NULL) {
            *out_handle = NULL;
        }
        return pdFAIL;
    }
    pthread_detach(t->thread);
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    usleep((useconds_t)ticks * 1000);
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait)
{
    pthread_once(&s_task_key_once, task_key_init);
    struct sim_task *t = pthread_getspecific(s_task_key);
    if (t == NULL) {
        usleep((useconds_t)ticks_to_wait * 1000);  // not a sim task: plain sleep
        return 0;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ticks_to_wait / 1000;
    deadline.tv_nsec += (long)(ticks_to_wait % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&t->mutex);
    while (t->notify_count == 0) {
        if (ticks_to_wait == portMAX_DELAY) {
            pthread_cond_wait(&t->cond, &t->mutex);
        } else if (pthread_cond_timedwait(&t->cond, &t->mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    uint32_t value = t->notify_count;
    t->notify_count = clear_on_exit ? 0 : (value > 0 ? value - 1 : 0);
    pthread_mutex_unlock(&t->mutex);
    return value;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    if (task == NULL) {
        return;
    }
    pthread_mutex_lock(&task->mutex);
    task->notify_count++;
    pthread_cond_signal(&task->cond);
    pthread_mutex_unlock(&task->mutex);
}

// ------------------------------------------------ fake flash dir + path remap

static const char *sim_fs_root(void)
{
    static char root[512];
    if (root[0] == '\0') {
        const char *env = getenv("DWT_SIM_FS");
        snprintf(root, sizeof(root), "%s", env != NULL ? env : SIM_FS_ROOT);
        mkdir(root, 0755);
    }
    return root;
}

// Device mount points -> host dirs under the sim root; else pass through.
// /storage = the LittleFS preview cache, /sdcard = the pattern TF card
// (drop tools/make_pattern_sd.py output into sim/simfs/sdcard to test it).
static const char *remap(const char *path, char *buf, size_t cap)
{
    static const char *mounts[] = { "/storage", "/sdcard" };
    for (size_t i = 0; path != NULL && i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        size_t n = strlen(mounts[i]);
        if (strncmp(path, mounts[i], n) == 0 && (path[n] == '\0' || path[n] == '/')) {
            snprintf(buf, cap, "%s%s", sim_fs_root(), path);
            return buf;
        }
    }
    return path;
}

FILE *sim_fopen(const char *path, const char *mode)
{
    char buf[768];
    return fopen(remap(path, buf, sizeof(buf)), mode);
}

int sim_stat(const char *path, struct stat *st)
{
    char buf[768];
    return stat(remap(path, buf, sizeof(buf)), st);
}

int sim_unlink(const char *path)
{
    char buf[768];
    return unlink(remap(path, buf, sizeof(buf)));
}

int sim_rename(const char *from, const char *to)
{
    char a[768], b[768];
    return rename(remap(from, a, sizeof(a)), remap(to, b, sizeof(b)));
}

int sim_mkdir(const char *path, unsigned short mode)
{
    char buf[768];
    return mkdir(remap(path, buf, sizeof(buf)), mode);
}

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    char buf[768];
    const char *dir = remap(conf->base_path, buf, sizeof(buf));
    mkdir(dir, 0755);
    return ESP_OK;
}

esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes,
                            size_t *used_bytes)
{
    (void)partition_label;
    *total_bytes = 10 * 1024 * 1024;  // partitions.csv: storage = 10 MB
    *used_bytes = 0;
    return ESP_OK;
}

// ------------------------------------------------------------ file-backed NVS
// One namespace per file: <root>/nvs_<ns>.txt, "key=value" lines. The whole
// store is small (a dozen keys), so open loads it and commit rewrites it.

#define NVS_MAX_ENTRIES 32

struct sim_nvs {
    char path[600];
    int rw;
    int count;
    char keys[NVS_MAX_ENTRIES][24];
    char values[NVS_MAX_ENTRIES][192];
};

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    struct sim_nvs *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(h->path, sizeof(h->path), "%s/nvs_%s.txt", sim_fs_root(), ns);
    h->rw = (mode == NVS_READWRITE);

    FILE *f = fopen(h->path, "r");
    if (f == NULL) {
        if (mode == NVS_READONLY) {
            free(h);
            return ESP_ERR_NVS_NOT_FOUND;  // namespace not created yet
        }
    } else {
        char line[256];
        while (h->count < NVS_MAX_ENTRIES && fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            char *nl = strchr(line, '\n');
            if (nl != NULL) {
                *nl = '\0';
            }
            if (eq == NULL) {
                continue;
            }
            *eq = '\0';
            snprintf(h->keys[h->count], sizeof(h->keys[0]), "%s", line);
            snprintf(h->values[h->count], sizeof(h->values[0]), "%s", eq + 1);
            h->count++;
        }
        fclose(f);
    }
    *out = h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    free(h);
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    if (!h->rw) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(h->path, "w");
    if (f == NULL) {
        return ESP_FAIL;
    }
    for (int i = 0; i < h->count; i++) {
        fprintf(f, "%s=%s\n", h->keys[i], h->values[i]);
    }
    fclose(f);
    return ESP_OK;
}

static const char *nvs_find(struct sim_nvs *h, const char *key)
{
    for (int i = 0; i < h->count; i++) {
        if (strcmp(h->keys[i], key) == 0) {
            return h->values[i];
        }
    }
    return NULL;
}

static esp_err_t nvs_put(struct sim_nvs *h, const char *key, const char *value)
{
    for (int i = 0; i < h->count; i++) {
        if (strcmp(h->keys[i], key) == 0) {
            snprintf(h->values[i], sizeof(h->values[0]), "%s", value);
            return ESP_OK;
        }
    }
    if (h->count >= NVS_MAX_ENTRIES) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(h->keys[h->count], sizeof(h->keys[0]), "%s", key);
    snprintf(h->values[h->count], sizeof(h->values[0]), "%s", value);
    h->count++;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    const char *v = nvs_find(h, key);
    if (v == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t need = strlen(v) + 1;
    if (out != NULL) {
        if (*len < need) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, v, need);
    }
    *len = need;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    return nvs_put(h, key, value);
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    const char *v = nvs_find(h, key);
    if (v == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = (uint32_t)strtoul(v, NULL, 10);
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", value);
    return nvs_put(h, key, buf);
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    uint32_t v;
    esp_err_t err = nvs_get_u32(h, key, &v);
    if (err == ESP_OK) {
        *out = (uint8_t)v;
    }
    return err;
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    return nvs_set_u32(h, key, value);
}

// --------------------------------------------- esp_http_client over sockets

struct sim_http {
    esp_http_client_config_t cfg;
    esp_http_client_method_t method;
    char host[128];
    int port;
    char path[512];
    char headers[512];  // extra request headers, CRLF-joined
    int fd;
    int status;
    long long content_length;  // -1 = unknown (read to EOF)
    long long body_read;
    char rxbuf[4096];  // bytes read past the header terminator
    int rxlen, rxpos;
};

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    struct sim_http *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->cfg = *cfg;
    c->fd = -1;
    c->content_length = -1;

    // Parse http://host[:port]/path
    const char *u = cfg->url;
    if (strncmp(u, "http://", 7) != 0) {
        free(c);
        return NULL;
    }
    u += 7;
    const char *slash = strchr(u, '/');
    const char *hostend = slash != NULL ? slash : u + strlen(u);
    const char *colon = memchr(u, ':', (size_t)(hostend - u));
    if (colon != NULL) {
        c->port = atoi(colon + 1);
        snprintf(c->host, sizeof(c->host), "%.*s", (int)(colon - u), u);
    } else {
        c->port = 80;
        snprintf(c->host, sizeof(c->host), "%.*s", (int)(hostend - u), u);
    }
    snprintf(c->path, sizeof(c->path), "%s", slash != NULL ? slash : "/");
    return c;
}

esp_err_t esp_http_client_set_method(esp_http_client_handle_t c,
                                     esp_http_client_method_t method)
{
    c->method = method;
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,
                                     const char *key, const char *value)
{
    size_t used = strlen(c->headers);
    snprintf(c->headers + used, sizeof(c->headers) - used, "%s: %s\r\n", key, value);
    return ESP_OK;
}

static void http_set_timeout(struct sim_http *c)
{
    struct timeval tv = {
        .tv_sec = c->cfg.timeout_ms / 1000,
        .tv_usec = (c->cfg.timeout_ms % 1000) * 1000,
    };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", c->port);
    if (getaddrinfo(c->host, portstr, &hints, &res) != 0 || res == NULL) {
        return ESP_FAIL;
    }
    c->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c->fd < 0) {
        freeaddrinfo(res);
        return ESP_FAIL;
    }
    http_set_timeout(c);
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(c->fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        close(c->fd);
        c->fd = -1;
        return ESP_FAIL;
    }
    freeaddrinfo(res);

    char req[1600];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "Connection: close\r\n"
                     "%s",
                     c->method == HTTP_METHOD_POST ? "POST" : "GET",
                     c->path, c->host, c->port, c->headers);
    if (c->method == HTTP_METHOD_POST || write_len > 0) {
        n += snprintf(req + n, sizeof(req) - (size_t)n,
                      "Content-Length: %d\r\n", write_len);
    }
    n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
    if (n >= (int)sizeof(req) || send(c->fd, req, (size_t)n, 0) != n) {
        close(c->fd);
        c->fd = -1;
        return ESP_FAIL;
    }
    return ESP_OK;
}

int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len)
{
    int total = 0;
    while (total < len) {
        ssize_t n = send(c->fd, buf + total, (size_t)(len - total), 0);
        if (n <= 0) {
            return total;
        }
        total += (int)n;
    }
    return total;
}

// Reads the status line + headers; fires ON_HEADER events; buffers any body
// bytes that arrived with the headers. Returns content-length (0 if none
// advertised), or negative on error (-ESP_ERR_HTTP_EAGAIN for timeout).
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    char hdr[8192];
    int used = 0;
    char *end = NULL;
    while (used < (int)sizeof(hdr) - 1) {
        ssize_t n = recv(c->fd, hdr + used, sizeof(hdr) - 1 - (size_t)used, 0);
        if (n < 0) {
            return (errno == EWOULDBLOCK || errno == EAGAIN)
                       ? -(int64_t)ESP_ERR_HTTP_EAGAIN
                       : -1;
        }
        if (n == 0) {
            return -1;  // closed before headers finished
        }
        used += (int)n;
        hdr[used] = '\0';
        end = strstr(hdr, "\r\n\r\n");
        if (end != NULL) {
            break;
        }
    }
    if (end == NULL) {
        return -1;
    }

    // Stash body bytes read past the terminator.
    int body_off = (int)(end - hdr) + 4;
    c->rxlen = used - body_off;
    if (c->rxlen > 0) {
        memcpy(c->rxbuf, hdr + body_off, (size_t)c->rxlen);
    }
    c->rxpos = 0;
    end[2] = '\0';  // terminate after the last header CRLF

    if (sscanf(hdr, "HTTP/%*d.%*d %d", &c->status) != 1) {
        return -1;
    }

    // Walk header lines: capture Content-Length, forward each to the handler.
    c->content_length = 0;  // matches esp_http_client: 0 when not advertised
    char *line = strstr(hdr, "\r\n");
    while (line != NULL) {
        line += 2;
        char *next = strstr(line, "\r\n");
        if (next == NULL || next == line) {
            break;
        }
        *next = '\0';
        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *value = colon + 1;
            while (*value == ' ') {
                value++;
            }
            if (strcasecmp(line, "Content-Length") == 0) {
                c->content_length = atoll(value);
            }
            if (c->cfg.event_handler != NULL) {
                esp_http_client_event_t evt = {
                    .event_id = HTTP_EVENT_ON_HEADER,
                    .user_data = c->cfg.user_data,
                    .header_key = line,
                    .header_value = value,
                };
                c->cfg.event_handler(&evt);
            }
        }
        *next = '\r';  // restore so strstr keeps walking
        line = next;
    }
    return c->content_length;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    return c->status;
}

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    if (c->content_length >= 0 && c->body_read >= c->content_length) {
        return 0;
    }
    if (c->rxpos < c->rxlen) {
        int n = c->rxlen - c->rxpos;
        if (n > len) {
            n = len;
        }
        memcpy(buf, c->rxbuf + c->rxpos, (size_t)n);
        c->rxpos += n;
        c->body_read += n;
        return n;
    }
    ssize_t n = recv(c->fd, buf, (size_t)len, 0);
    if (n < 0) {
        return (errno == EWOULDBLOCK || errno == EAGAIN)
                   ? -(int)ESP_ERR_HTTP_EAGAIN
                   : -1;
    }
    c->body_read += n;
    return (int)n;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    esp_http_client_close(c);
    free(c);
    return ESP_OK;
}
