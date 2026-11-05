#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdlog.h"
#include "wifi_creds.h"
#include "harden.h"

static const char *TAG = "wifi_creds";
#define NVS_NS   "wifi"
#define NVS_KEY  "nets"
#define CSV_PATH SDLOG_DIR "/wifi.csv"

typedef struct { char ssid[33]; char password[65]; } stored_t;   /* what goes to NVS */

static wifi_cred_t s_creds[WIFI_CREDS_MAX];
static size_t      s_n;

static bool persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    stored_t st[WIFI_CREDS_MAX];
    memset(st, 0, sizeof st);
    for (size_t i = 0; i < s_n; i++) {
        strlcpy(st[i].ssid, s_creds[i].ssid, sizeof st[i].ssid);
        strlcpy(st[i].password, s_creds[i].password, sizeof st[i].password);
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY, st, sizeof(stored_t) * s_n);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static void load_nvs(void)
{
    nvs_handle_t h;
    s_n = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    stored_t st[WIFI_CREDS_MAX];
    size_t len = sizeof st;
    if (nvs_get_blob(h, NVS_KEY, st, &len) == ESP_OK) {
        size_t n = len / sizeof(stored_t);
        for (size_t i = 0; i < n && i < WIFI_CREDS_MAX; i++) {
            memset(&s_creds[s_n], 0, sizeof s_creds[s_n]);
            strlcpy(s_creds[s_n].ssid, st[i].ssid, sizeof s_creds[s_n].ssid);
            strlcpy(s_creds[s_n].password, st[i].password, sizeof s_creds[s_n].password);
            s_creds[s_n].last_rssi = -127;
            s_n++;
        }
    }
    nvs_close(h);
}

/* config.json can add networks before wifi_creds_load() runs: load the NVS list first so persist() never overwrites it. */
static bool s_loaded;
static void ensure_loaded(void)
{
    if (!s_loaded) { load_nvs(); s_loaded = true; }
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

/* /sdcard/obd/wifi.csv: "ssid,password" per line, '#' comments. A line "-ssid" removes a network. */
static void import_csv(void)
{
    FILE *f = fopen(CSV_PATH, "r");
    if (!f) return;
    char line[160];
    int added = 0, removed = 0;
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        if (len && line[len - 1] != '\n' && !feof(f)) {         /* line longer than the buffer: drop all of it, not just the head */
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            ESP_LOGW(TAG, "skipping over-long line");
            continue;
        }
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '-') { if (wifi_creds_remove(line + 1)) removed++; continue; }
        char *comma = strchr(line, ',');
        const char *pass = "";
        if (comma) { *comma = 0; pass = comma + 1; }
        char ssid[64];
        strlcpy(ssid, line, sizeof ssid);
        trim(ssid);
        if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) { ESP_LOGW(TAG, "skipping invalid line"); continue; }
        if (wifi_creds_add(ssid, pass)) added++;
    }
    fclose(f);
    if (remove(CSV_PATH) == 0) ESP_LOGI(TAG, "imported %d network(s), removed %d; wifi.csv deleted from the card", added, removed);
    else ESP_LOGW(TAG, "imported %d network(s) but could not delete wifi.csv", added);
}

static void write_example(void)
{
    FILE *f = fopen(SDLOG_DIR "/wifi.example.csv", "w");
    if (!f) return;
    fputs("# OpenCarMind Wi-Fi networks. Rename this file to wifi.csv; it is imported at boot and then deleted.\n"
          "# One network per line: ssid,password   (2.4 GHz networks only; the ESP32-S3 has no 5 GHz radio)\n"
          "# A line starting with '-' forgets a network:  -Old Network Name\n"
          "My Home Wifi,my-wifi-password\n"
          "Phone Hotspot,my-hotspot-password\n", f);
    fclose(f);
}

size_t wifi_creds_load(void)
{
    ensure_loaded();
    /* Kconfig fallback: keeps an existing build working, but NVS is the source of truth */
#if CONFIG_OCM_WIFI_CLOCK_SYNC
    if (CONFIG_SYNC_WIFI_SSID[0] && !wifi_creds_find(CONFIG_SYNC_WIFI_SSID)) wifi_creds_add(CONFIG_SYNC_WIFI_SSID, CONFIG_SYNC_WIFI_PASSWORD);
#endif
    if (sdlog_ok()) {
        import_csv();
        if (s_n == 0) write_example();
    }
    ESP_LOGI(TAG, "%u known network(s)", (unsigned)s_n);
    return s_n;
}

size_t wifi_creds_count(void) { ensure_loaded(); return s_n; }
wifi_cred_t *wifi_creds_get(size_t i) { ensure_loaded(); return i < s_n ? &s_creds[i] : NULL; }

wifi_cred_t *wifi_creds_find(const char *ssid)
{
    ensure_loaded();
    for (size_t i = 0; i < s_n; i++) if (strcmp(s_creds[i].ssid, ssid) == 0) return &s_creds[i];
    return NULL;
}

bool wifi_creds_add(const char *ssid, const char *password)
{
    if (!harden_wifi_cred_ok(ssid, password)) { ESP_LOGW(TAG, "network entry rejected: SSID 1-32 bytes, password empty or 8-64 bytes, no control characters"); return false; }
    wifi_cred_t *c = wifi_creds_find(ssid);
    if (!c) {
        if (s_n >= WIFI_CREDS_MAX) { ESP_LOGW(TAG, "network list full (%d)", WIFI_CREDS_MAX); return false; }
        c = &s_creds[s_n++];
        memset(c, 0, sizeof *c);
        strlcpy(c->ssid, ssid, sizeof c->ssid);
        c->last_rssi = -127;
    }
    strlcpy(c->password, password, sizeof c->password);
    c->fail_count = 0;
    c->retry_after_ms = 0;
    return persist();
}

static void fold(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 1 < n; in++) if (!isspace((unsigned char)*in)) out[k++] = (char)tolower((unsigned char)*in);
    out[k] = 0;
}

void wifi_creds_dedupe(const char *good_ssid)
{
    char a[40], b[40];
    fold(good_ssid, a, sizeof a);
    for (size_t i = 0; i < s_n; ) {
        fold(s_creds[i].ssid, b, sizeof b);
        if (strcmp(s_creds[i].ssid, good_ssid) != 0 && strcmp(a, b) == 0) {
            ESP_LOGI(TAG, "removing a near-duplicate saved network");
            wifi_creds_remove(s_creds[i].ssid);
        } else i++;
    }
}

bool wifi_creds_remove(const char *ssid)
{
    ensure_loaded();
    for (size_t i = 0; i < s_n; i++) {
        if (strcmp(s_creds[i].ssid, ssid) != 0) continue;
        memmove(&s_creds[i], &s_creds[i + 1], (s_n - i - 1) * sizeof s_creds[0]);
        s_n--;
        return persist();
    }
    return false;
}
