#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "cJSON.h"
#include "sdlog.h"
#include "sys_mode.h"
#include "events.h"
#include "version.h"
#include "uploader.h"

static const char *TAG = "upload";

#define PAIR_PATH     SDLOG_DIR "/receiver.json"
#define CERT_MAX      2048
#define LIST_MAX      (64 * 1024)
#define CHUNK         (8 * 1024)
#define PASS_BUDGET   (256 * 1024)     /* bytes per pass, so one pass stays a few seconds long */

static char s_host[64], s_token[72], s_board[13];
static uint16_t s_port;
static char *s_cert;                    /* PEM, PSRAM */
static bool s_ok;

typedef struct { char *buf; size_t len, cap; } body_t;

static bool host_ok(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n >= sizeof s_host) return false;
    for (; *h; h++) if (!isalnum((unsigned char)*h) && *h != '.' && *h != '-' && *h != ':') return false;
    return true;
}

static bool token_ok(const char *t)
{
    size_t n = strlen(t);
    if (n < 32 || n >= sizeof s_token) return false;
    for (; *t; t++) if (!isalnum((unsigned char)*t) && *t != '-' && *t != '_') return false;
    return true;
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("rcv", NVS_READONLY, &h) != ESP_OK) return;
    size_t a = sizeof s_host, b = sizeof s_token, c = CERT_MAX;
    if (!s_cert) s_cert = heap_caps_calloc(1, CERT_MAX, MALLOC_CAP_SPIRAM);
    s_ok = s_cert && nvs_get_str(h, "host", s_host, &a) == ESP_OK && nvs_get_u16(h, "port", &s_port) == ESP_OK &&
           nvs_get_str(h, "token", s_token, &b) == ESP_OK && nvs_get_str(h, "cert", s_cert, &c) == ESP_OK;
    nvs_close(h);
}

static void import_file(void)
{
    if (!sdlog_ok()) return;
    char *txt = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM);
    if (!txt) return;
    if (!sdlog_load_text(PAIR_PATH, txt, 4096)) { free(txt); return; }
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    cJSON *host = cJSON_GetObjectItem(j, "host"), *port = cJSON_GetObjectItem(j, "port"),
          *token = cJSON_GetObjectItem(j, "token"), *cert = cJSON_GetObjectItem(j, "cert");
    bool valid = cJSON_IsString(host) && host_ok(host->valuestring) && cJSON_IsNumber(port) && port->valuedouble >= 1 &&
                 port->valuedouble <= 65535 && cJSON_IsString(token) && token_ok(token->valuestring) && cJSON_IsString(cert) &&
                 strncmp(cert->valuestring, "-----BEGIN CERTIFICATE-----", 27) == 0 && strlen(cert->valuestring) < CERT_MAX;
    if (valid) {
        nvs_handle_t h;
        if (nvs_open("rcv", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "host", host->valuestring);
            nvs_set_u16(h, "port", (uint16_t)port->valuedouble);
            nvs_set_str(h, "token", token->valuestring);
            nvs_set_str(h, "cert", cert->valuestring);
            nvs_commit(h);
            nvs_close(h);
            sys_event(EV_INFO, "Paired with the desktop app at %s", host->valuestring);
        }
    } else sys_event(EV_WARN, "receiver.json is not valid: ignored");
    cJSON_Delete(j);
    remove(PAIR_PATH);                      /* the token should not stay on the card */
}

void uploader_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_board, sizeof s_board, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    import_file();
    load_nvs();
    if (s_ok) ESP_LOGI(TAG, "uploads go to the desktop app at %s:%u", s_host, s_port);
}

bool uploader_configured(void) { return s_ok; }

static esp_err_t on_event(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b && b->buf) {
        size_t n = (size_t)e->data_len;
        if (b->len + n >= b->cap) n = b->cap - b->len - 1;
        memcpy(b->buf + b->len, e->data, n);
        b->len += n;
        b->buf[b->len] = 0;
    }
    return ESP_OK;
}

/* One request on the shared keep-alive client. Returns the HTTP status, or -1 when the desktop could not be reached. */
static int request(esp_http_client_handle_t c, body_t *resp, esp_http_client_method_t method, const char *path,
                   const char *body, int body_len)
{
    char url[160];
    snprintf(url, sizeof url, "https://%s:%u%s", s_host, s_port, path);
    if (resp) { resp->len = 0; if (resp->buf) resp->buf[0] = 0; }
    esp_http_client_set_url(c, url);
    esp_http_client_set_method(c, method);
    esp_http_client_set_post_field(c, body, body ? body_len : 0);
    if (esp_http_client_perform(c) != ESP_OK) return -1;
    return esp_http_client_get_status_code(c);
}

static bool valid_name(const char *n)
{
    if (strlen(n) < 11 || n[0] != 's') return false;
    for (int i = 1; i <= 6; i++) if (!isdigit((unsigned char)n[i])) return false;
    const char *ext = n + 7;
    return !strcmp(ext, ".csv") || !strcmp(ext, ".ml.csv") || !strcmp(ext, ".pat") || !strcmp(ext, ".raw") || !strcmp(ext, ".json");
}

/* Size the desktop holds for `name`, or 0 when it has none. The list is "name size\n" lines. */
static long long remote_size(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; p && *p; ) {
        if (strncmp(p, name, n) == 0 && p[n] == ' ') return atoll(p + n + 1);
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

bool uploader_pass(void)
{
    if (!s_ok) return false;
    static body_t list, small;
    if (!list.buf) { list.cap = LIST_MAX; list.buf = heap_caps_malloc(LIST_MAX, MALLOC_CAP_SPIRAM); }
    if (!small.buf) { small.cap = 64; small.buf = heap_caps_malloc(64, MALLOC_CAP_SPIRAM); }
    char *chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!list.buf || !small.buf || !chunk) { free(chunk); return false; }

    char auth[96];
    snprintf(auth, sizeof auth, "Bearer %s", s_token);
    esp_http_client_config_t cfg = {
        .url = "https://localhost/",
        .cert_pem = s_cert,
        .skip_cert_common_name_check = true,   /* the certificate itself is pinned: it is the only trusted one */
        .timeout_ms = 8000,
        .keep_alive_enable = true,
        .event_handler = on_event,
        .user_data = &list,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(chunk); return false; }
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "X-OCM-Board", s_board);
    esp_http_client_set_header(c, "X-OCM-Firmware", CARMIND_VERSION);

    bool reached = false;
    int status = request(c, &list, HTTP_METHOD_GET, "/api/v1/files", NULL, 0);
    if (status == 200) {
        reached = true;
        esp_http_client_set_user_data(c, &small);
        size_t budget = PASS_BUDGET;
        char current[16];
        snprintf(current, sizeof current, "s%06lu.json", (unsigned long)sdlog_session_id());
        DIR *d = opendir(SDLOG_SESSIONS_DIR);
        struct dirent *e;
        while (d && budget > 0 && (e = readdir(d)) != NULL && sys_mode_get() != MODE_DRIVE) {
            if (!valid_name(e->d_name)) continue;
            char path[96], q[96];
            snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%.20s", e->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            long long have = remote_size(list.buf, e->d_name), size = st.st_size;
            bool json = strstr(e->d_name, ".json") != NULL;
            if (json) {
                if (size == have && strcmp(e->d_name, current) != 0) continue;
                if (size > CHUNK) continue;      /* session JSON stays a few KB */
                FILE *f = fopen(path, "rb");
                if (!f) continue;
                size_t n = fread(chunk, 1, (size_t)size, f);
                fclose(f);
                snprintf(q, sizeof q, "/api/v1/put?name=%.20s", e->d_name);
                if (request(c, &small, HTTP_METHOD_PUT, q, chunk, (int)n) != 200) { reached = false; break; }
                budget = budget > n ? budget - n : 0;
                continue;
            }
            if (have > size) { ESP_LOGW(TAG, "desktop has more of %s than the card (%lld > %lld): skipped", e->d_name, have, size); continue; }
            FILE *f = have < size ? fopen(path, "rb") : NULL;
            while (f && have < size && budget > 0 && sys_mode_get() != MODE_DRIVE) {
                size_t want = (size_t)(size - have) < CHUNK ? (size_t)(size - have) : CHUNK;
                if (fseek(f, (long)have, SEEK_SET) != 0) break;
                size_t n = fread(chunk, 1, want, f);
                if (n == 0) break;
                snprintf(q, sizeof q, "/api/v1/append?name=%.20s&offset=%lld", e->d_name, have);
                int s = request(c, &small, HTTP_METHOD_POST, q, chunk, (int)n);
                if (s == 200) { have += (long long)n; budget = budget > n ? budget - n : 0; }
                else if (s == 409) have = atoll(small.buf);       /* the desktop says where it is: continue from there */
                else { reached = s > 0; break; }
            }
            if (f) fclose(f);
        }
        if (d) closedir(d);
    } else if (status == 401) sys_event(EV_WARN, "Desktop app refused the board: pair it again");
    esp_http_client_cleanup(c);
    free(chunk);
    return reached;
}
