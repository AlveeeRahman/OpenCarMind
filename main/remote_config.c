#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "remote_config.h"
#include "wifi_creds.h"
#include "sdlog.h"
#include "events.h"

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
    ESP_LOGI(TAG, "rev %lu: poll %u ms, slow every %u, dtc %u s, backlight %u%%",
             (unsigned long)s_cfg.rev, s_cfg.poll_interval_ms, s_cfg.slow_group_every, s_cfg.dtc_interval_s, s_cfg.backlight_pct);
}

const remote_config_t *remote_config_get(void) { return &s_cfg; }
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
