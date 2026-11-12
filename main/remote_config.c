#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "remote_config.h"
#include "wifi_creds.h"
#include "sdlog.h"
#include "events.h"
#include <stdlib.h>
#include <time.h>
#include "harden.h"

static void time_host_load(void);
#include "units.h"

static const char *TAG = "rconf";
static remote_config_t s_cfg;

static void defaults(void)
{
    s_cfg.rev = 0;
    s_cfg.poll_interval_ms = CONFIG_OBD_POLL_INTERVAL_MS;
    s_cfg.slow_group_every = CONFIG_OBD_SLOW_GROUP_EVERY;
    s_cfg.dtc_interval_s = CONFIG_OBD_DTC_INTERVAL_S;
    s_cfg.backlight_pct = CONFIG_LCD_BACKLIGHT_PCT;
    s_cfg.theme_mode = 0;
    s_cfg.day_start_hour = 6;
    s_cfg.day_end_hour = 17;
    s_cfg.timezone[0] = 0;
}

/* Local time drives the light and dark theme (06:00 to 17:00 light by default) and the clock on screen. */
static void apply_timezone(void)
{
    setenv("TZ", s_cfg.timezone[0] ? s_cfg.timezone : CONFIG_SYNC_TIMEZONE, 1);
    tzset();
}

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open("rconf", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &s_cfg, sizeof s_cfg);
    nvs_commit(h);
    nvs_close(h);
}

void remote_config_init(void)
{
    defaults();
    nvs_handle_t h;
    if (nvs_open("rconf", NVS_READONLY, &h) == ESP_OK) {
        remote_config_t t;
        size_t len = sizeof t;
        if (nvs_get_blob(h, "cfg", &t, &len) == ESP_OK && len == sizeof t) s_cfg = t;
        nvs_close(h);
    }
    s_cfg.timezone[sizeof s_cfg.timezone - 1] = 0;
    if (s_cfg.timezone[0] && !harden_timezone_ok(s_cfg.timezone)) s_cfg.timezone[0] = 0;
    apply_timezone();
    time_host_load();
    ESP_LOGI(TAG, "rev %lu: poll %u ms, slow every %u, dtc %u s, backlight %u%%",
             (unsigned long)s_cfg.rev, s_cfg.poll_interval_ms, s_cfg.slow_group_every, s_cfg.dtc_interval_s, s_cfg.backlight_pct);
}

const remote_config_t *remote_config_get(void) { return &s_cfg; }

static char s_time_host[64];                         /* empty = the build default */

#ifndef CONFIG_OCM_TIME_HTTPS_HOST
#define CONFIG_OCM_TIME_HTTPS_HOST "www.google.com"
#endif

const char *remote_config_time_host(void) { return s_time_host[0] ? s_time_host : CONFIG_OCM_TIME_HTTPS_HOST; }

static void time_host_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof s_time_host;
    s_time_host[0] = 0;
    if (nvs_open("rconf", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "thost", s_time_host, &len) != ESP_OK) s_time_host[0] = 0;
        nvs_close(h);
    }
    if (s_time_host[0] && !harden_hostname_ok(s_time_host)) s_time_host[0] = 0;
}

static bool time_host_store(const char *host)
{
    nvs_handle_t h;
    if (nvs_open("rconf", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (host[0] ? nvs_set_str(h, "thost", host) : (nvs_erase_key(h, "thost"), ESP_OK)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) strlcpy(s_time_host, host, sizeof s_time_host);
    return ok;
}
uint32_t remote_config_rev(void) { return s_cfg.rev; }

static bool num_in(cJSON *root, const char *key, double lo, double hi, double *out)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(v)) return false;
    if (v->valuedouble < lo || v->valuedouble > hi) { ESP_LOGW(TAG, "%s out of range", key); return false; }
    *out = v->valuedouble;
    return true;
}

int remote_config_apply(cJSON *root)
{
    int applied = 0;
    double d;
    if (num_in(root, "poll_interval_ms", 50, 5000, &d))  { s_cfg.poll_interval_ms = (uint16_t)d; applied++; }
    if (num_in(root, "slow_group_every", 1, 50, &d))     { s_cfg.slow_group_every = (uint8_t)d; applied++; }
    if (num_in(root, "dtc_interval_s", 10, 3600, &d))    { s_cfg.dtc_interval_s = (uint16_t)d; applied++; }
    if (num_in(root, "backlight_pct", 5, 100, &d))       { s_cfg.backlight_pct = (uint8_t)d; applied++; }
    if (num_in(root, "theme_mode", 0, 2, &d))            { s_cfg.theme_mode = (uint8_t)d; applied++; }
    if (num_in(root, "day_start_hour", 0, 23, &d))       { s_cfg.day_start_hour = (uint8_t)d; applied++; }
    if (num_in(root, "day_end_hour", 0, 23, &d))         { s_cfg.day_end_hour = (uint8_t)d; applied++; }

    cJSON *tz = cJSON_GetObjectItem(root, "timezone");
    if (cJSON_IsString(tz)) {
        if (harden_timezone_ok(tz->valuestring)) {
            strlcpy(s_cfg.timezone, tz->valuestring, sizeof s_cfg.timezone);
            apply_timezone();
            applied++;
        } else ESP_LOGW(TAG, "timezone not valid: ignored");
    }

    cJSON *th = cJSON_GetObjectItem(root, "time_host");
    if (cJSON_IsString(th)) {
        /* "" goes back to the build default */
        if ((!th->valuestring[0] || harden_hostname_ok(th->valuestring)) && time_host_store(th->valuestring)) {
            ESP_LOGI(TAG, "time server: %s", remote_config_time_host());
            applied++;
        } else ESP_LOGW(TAG, "time_host not valid: ignored");
    }

    cJSON *units = cJSON_GetObjectItem(root, "units");
    if (cJSON_IsString(units)) {
        if (!strcmp(units->valuestring, "auto") || units_parse(units->valuestring) >= 0) {
            units_set_override(units_parse(units->valuestring));
            applied++;
        } else ESP_LOGW(TAG, "units not valid: use metric, us, uk or auto");
    }

    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsArray(wifi)) {
        cJSON *e;
        cJSON_ArrayForEach(e, wifi) {
            cJSON *ssid = cJSON_GetObjectItem(e, "ssid"), *pw = cJSON_GetObjectItem(e, "password"), *forget = cJSON_GetObjectItem(e, "forget");
            if (!cJSON_IsString(ssid)) continue;
            if (cJSON_IsTrue(forget)) { if (wifi_creds_remove(ssid->valuestring)) applied++; }
            else if (wifi_creds_add(ssid->valuestring, cJSON_IsString(pw) ? pw->valuestring : "")) applied++;
        }
    }
    cJSON *pids = cJSON_GetObjectItem(root, "custom_pids");
    if (cJSON_IsString(pids) && sdlog_ok()) {
        if (sdlog_save_text_atomic(SDLOG_DIR "/custom_pids.csv", pids->valuestring)) applied++;
    }
    cJSON *rev = cJSON_GetObjectItem(root, "rev");
    if (cJSON_IsNumber(rev) && rev->valuedouble >= 0 && rev->valuedouble <= 4294967295.0) s_cfg.rev = (uint32_t)rev->valuedouble;
    persist();
    return applied;
}

#define SD_CONFIG_PATH   SDLOG_DIR "/config.json"
#define SD_CONFIG_MAX    4096

void remote_config_load_sd(void)
{
    if (!sdlog_ok()) return;
    FILE *fp = fopen(SD_CONFIG_PATH, "r");
    if (!fp) return;                                   /* no file: keep the NVS / Kconfig values */
    static char buf[SD_CONFIG_MAX + 1];
    size_t n = fread(buf, 1, SD_CONFIG_MAX, fp);
    bool too_big = !feof(fp);
    fclose(fp);
    if (too_big) { sys_event(EV_WARN, "config.json larger than %d bytes: ignored", SD_CONFIG_MAX); return; }
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf);
    if (!cJSON_IsObject(root)) {
        sys_event(EV_WARN, "config.json is not valid JSON: ignored");
        cJSON_Delete(root);
        return;
    }
    int applied = remote_config_apply(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "config.json: %d setting(s) applied", applied);
    sys_event(EV_INFO, "config.json: %d setting(s) applied", applied);
}

#if CONFIG_IDF_TARGET_ESP32S31
/* ESP-Mosaico Settings > Clock: the zone picked by hand, stored like a CONFIG "timezone" (a later one replaces it) */
bool remote_config_set_timezone(const char *tz)
{
    if (!harden_timezone_ok(tz)) return false;
    strlcpy(s_cfg.timezone, tz, sizeof s_cfg.timezone);
    apply_timezone();
    persist();
    return true;
}
#endif
