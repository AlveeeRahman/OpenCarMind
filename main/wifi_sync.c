/* Wi-Fi clock sync. The board has no battery-backed clock, so session files would carry epoch 0 until some source
 * sets the time. This module scans for a known network, connects, sets the clock over SNTP, stays connected for
 * CONFIG_SYNC_STAY_CONNECTED_S seconds and then disconnects so the radio stops competing with BLE. It scans again
 * every CONFIG_SYNC_SCAN_INTERVAL_S seconds while the clock has never been set, and every
 * CONFIG_SYNC_RESYNC_INTERVAL_H hours afterwards to correct drift. It never scans or connects while the car is being
 * logged (sys_mode DRIVE), and drops an open connection when the drive starts.
 * Time comes from ONE server (remote_config_time_host(), default CONFIG_OCM_TIME_HTTPS_HOST): one HTTPS HEAD request,
 * the server's certificate checked against ESP-IDF's root bundle and its name, the time read from the Date header.
 * Apart from DHCP and the DNS lookup of that name, nothing else leaves the board; it listens on no port. The upload to
 * the desktop app is compiled out unless CONFIG_OCM_WIFI_UPLOAD (off in 2.5.x). */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "sys_mode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <strings.h>
#include "units.h"
#include "remote_config.h"
#include "esp_timer.h"
#include "wifi_sync.h"
#include "sdlog.h"
#include "esp_heap_caps.h"
#include "status.h"
#include "esp_attr.h"
#include "wifi_creds.h"
#include "harden.h"
#include "uploader.h"
#include "events.h"

static const char *TAG = "sync";

#if CONFIG_OCM_WIFI_CLOCK_SYNC


#define GOT_IP_BIT   BIT0
#define LOST_BIT     BIT1

static EventGroupHandle_t s_ev;
static bool s_connected;
static int  s_last_reason;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_last_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_connected = false;
        xEventGroupSetBits(s_ev, LOST_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xEventGroupSetBits(s_ev, GOT_IP_BIT);
    }
}

/* Scan everything once and pick the strongest known network that is not in a back-off period.
 * Logs one line per scan so a missing network is visible in the monitor. */
static wifi_cred_t *pick_network(int8_t *rssi)
{
    wifi_scan_config_t sc = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 60, .max = 120},
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { ESP_LOGW(TAG, "scan failed"); return NULL; }
    uint16_t n = 24;
    EXT_RAM_BSS_ATTR static wifi_ap_record_t ap[24];
    if (esp_wifi_scan_get_ap_records(&n, ap) != ESP_OK) { esp_wifi_clear_ap_list(); return NULL; }
    wifi_cred_t *best = NULL;
    int8_t best_rssi = -127;
    int64_t now_ms = esp_timer_get_time() / 1000;
    for (int i = 0; i < n; i++) {
        wifi_cred_t *c = wifi_creds_find((const char *)ap[i].ssid);
        if (!c) continue;
        c->last_rssi = ap[i].rssi;
        if (now_ms < c->retry_after_ms) continue;
        if (ap[i].rssi > best_rssi) { best_rssi = ap[i].rssi; best = c; }
    }
    /* network names stay out of the serial log; the screen shows the one the board joins */
    if (best) ESP_LOGI(TAG, "scan: %u networks, a known one in range, rssi %d", n, best_rssi);
    else {
        ESP_LOGI(TAG, "scan: %u networks, none of the %u known ones in range", n, (unsigned)wifi_creds_count());
        static int listed;
        if (listed++ % 10 == 0)      /* every 10th scan: signal and channel of what is visible (names stay out of the log) */
            for (int i = 0; i < n && i < 12; i++) ESP_LOGI(TAG, "   visible: rssi %d ch %u", ap[i].rssi, ap[i].primary);
    }
    if (!best || best_rssi < CONFIG_SYNC_WIFI_MIN_RSSI) return NULL;
    *rssi = best_rssi;
    return best;
}

static void apply_network(const wifi_cred_t *c)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, c->password, sizeof wc.sta.password);
    wc.sta.threshold.authmode = c->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.pmf_cfg.capable = true;        /* use protected management frames when the access point offers them */
    wc.sta.pmf_cfg.required = false;
    wc.sta.scan_method = WIFI_FAST_SCAN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static bool clock_is_set(void)
{
    STATUS_LOCK(); bool ok = g_status.time_synced; STATUS_UNLOCK();
    return ok;
}

static char s_date[40];                              /* Date header of the last reply; wifi task only */

static esp_err_t on_header(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && !strcasecmp(e->header_key, "Date"))
        strlcpy(s_date, e->header_value, sizeof s_date);
    return ESP_OK;
}

static bool sync_clock(void)
{
    const char *host = remote_config_time_host();
    char url[80];
    snprintf(url, sizeof url, "https://%s/", host);
    s_date[0] = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_HEAD,
        .crt_bundle_attach = esp_crt_bundle_attach,      /* chain to a known root, and the name must match the host */
        .event_handler = on_header,
        .timeout_ms = 10000,
        .disable_auto_redirect = true,                   /* the first reply already carries Date; never follow elsewhere */
        .keep_alive_enable = false,
    };
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = c ? esp_http_client_perform(c) : ESP_FAIL;
    int64_t rtt_us = esp_timer_get_time() - t0;
    if (c) esp_http_client_cleanup(c);
    int64_t epoch;
    if (err != ESP_OK || !harden_http_date(s_date, &epoch)) {
        sys_event(EV_WARN, "Clock sync from %s failed (%s)", host, err != ESP_OK ? esp_err_to_name(err) : "no Date");
        return false;
    }
    if (!harden_epoch_plausible(epoch)) {
        sys_event(EV_WARN, "Clock sync refused: %s gave %lld, outside 2026-2099", host, (long long)epoch);
        return false;
    }
    /* Date has 1 s resolution: take the middle of that second, stamped half a round trip before the reply arrived */
    int64_t us = epoch * 1000000LL + 500000 + rtt_us / 2;
    struct timeval tv = {.tv_sec = (time_t)(us / 1000000), .tv_usec = (suseconds_t)(us % 1000000)};
    settimeofday(&tv, NULL);
    int64_t epoch_ms = us / 1000;
    int64_t up_ms = esp_timer_get_time() / 1000;
    sdlog_meta_set_time_sync(epoch_ms, up_ms);
    STATUS_LOCK(); g_status.time_synced = true; g_status.clock_estimated = false; STATUS_UNLOCK();
    ESP_LOGI(TAG, "clock synced from %s, epoch %lld (round trip %lld ms)", host, (long long)epoch, (long long)(rtt_us / 1000));
    sys_event(EV_INFO, "Clock synced over Wi-Fi");
    return true;
}

static bool driving(void) { return sys_mode_get() == MODE_DRIVE; }

static void set_wifi_state(wifi_state_t st)
{
    STATUS_LOCK(); g_status.wifi = st; STATUS_UNLOCK();
}

/* Stay connected for CONFIG_SYNC_STAY_CONNECTED_S, ending early if the link drops or the drive starts. */
static void stay_connected(void)
{
    for (int s = 0; s < CONFIG_SYNC_STAY_CONNECTED_S && s_connected && !driving(); s++) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { STATUS_LOCK(); g_status.wifi_rssi = ap.rssi; STATUS_UNLOCK(); }
        EventBits_t w = xEventGroupWaitBits(s_ev, LOST_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (w & LOST_BIT) { sys_event(EV_WARN, "Wi-Fi lost (reason %d)", s_last_reason); break; }
    }
}

/* One attempt: scan, connect, set the clock, linger briefly, disconnect. Returns true when the clock was set. */
static bool sync_once(void)
{
    int8_t rssi = -127;
    set_wifi_state(WIFI_ST_SCANNING);
    wifi_cred_t *net = pick_network(&rssi);
    if (!net) return false;
    if (driving()) return false;

    ESP_LOGI(TAG, "connecting to a known network (rssi %d)", rssi);
    STATUS_LOCK();
    g_status.wifi = WIFI_ST_CONNECTING;
    g_status.wifi_rssi = rssi;
    snprintf(g_status.wifi_ssid, sizeof g_status.wifi_ssid, "%s", net->ssid);
    STATUS_UNLOCK();
    apply_network(net);
    xEventGroupClearBits(s_ev, GOT_IP_BIT | LOST_BIT);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, GOT_IP_BIT | LOST_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(b & GOT_IP_BIT)) {
        net->fail_count++;
        net->retry_after_ms = esp_timer_get_time() / 1000 + (net->fail_count >= 3 ? 300000 : 60000);   /* back off: 1 min, then 5 min */
        sys_event(EV_WARN, "Wi-Fi '%s' connect failed (%u)", net->ssid, net->fail_count);
        esp_wifi_disconnect();
        set_wifi_state(WIFI_ST_IDLE);
        return false;
    }
    net->fail_count = 0;
    wifi_creds_dedupe(net->ssid);

    set_wifi_state(WIFI_ST_CONNECTED);
    sys_event(EV_INFO, "Wi-Fi connected: %s", net->ssid);
    {
        /* first connect decides the display units from the country the access point broadcasts (802.11d) */
        wifi_ap_record_t ap = {0};
        char cc[3] = "";
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { cc[0] = ap.country.cc[0]; cc[1] = ap.country.cc[1]; cc[2] = 0; }
        units_detect(cc, remote_config_get()->timezone);
    }
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok && s_connected && !driving(); attempt++) ok = sync_clock();
    if (uploader_wifi_enabled()) {
        /* Paired with the desktop app: stay on Wi-Fi while parked and send new data every 10 s */
        int64_t last_resync_ms = esp_timer_get_time() / 1000;
        while (s_connected && !driving()) {
            uploader_pass();
            if (esp_timer_get_time() / 1000 - last_resync_ms > (int64_t)CONFIG_SYNC_RESYNC_INTERVAL_H * 3600000LL) {
                ok = sync_clock() || ok;
                last_resync_ms = esp_timer_get_time() / 1000;
            }
            EventBits_t w = xEventGroupWaitBits(s_ev, LOST_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
            if (w & LOST_BIT) { sys_event(EV_WARN, "Wi-Fi lost (reason %d)", s_last_reason); break; }
        }
    } else if (ok) stay_connected();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    set_wifi_state(WIFI_ST_IDLE);
    return ok;
}

static TaskHandle_t s_task;                        /* the sync task, woken early by wifi_sync_connect_now() */

static void sync_task(void *arg)
{
    /* A clock kept by the RTC through a software reset or deep sleep counts as set: the first drift correction is
       then a full resync interval away instead of a scan every few seconds. */
    uint32_t wait_s = clock_is_set() && !uploader_wifi_enabled() ? CONFIG_SYNC_RESYNC_INTERVAL_H * 3600U : 0;
    for (;;) {
        if (wait_s) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000U));   /* wifi_sync_connect_now() wakes it early */
        while (driving()) vTaskDelay(pdMS_TO_TICKS(2000));   /* never scan or connect while the car is being logged */
        bool ok = sync_once();
        if (uploader_wifi_enabled()) wait_s = CONFIG_SYNC_SCAN_INTERVAL_S;   /* paired: get back on Wi-Fi soon after parking */
        else if (ok) wait_s = CONFIG_SYNC_RESYNC_INTERVAL_H * 3600U;
        else    wait_s = clock_is_set() ? 900U : CONFIG_SYNC_SCAN_INTERVAL_S;   /* due for drift, network not in reach: ask again in 15 min */
    }
}

bool wifi_sync_connected(void) { return s_connected; }

bool wifi_sync_connect_now(void)
{
    if (!s_task) return false;                       /* no known network at boot: the task never started */
    xTaskNotifyGive(s_task);
    return true;
}

esp_err_t wifi_sync_start(void)
{
    if (wifi_creds_load() == 0) {
        ESP_LOGW(TAG, "no Wi-Fi networks known: put wifi.csv on the SD card (see docs); the clock stays unset");
        set_wifi_state(WIFI_ST_DISABLED);
        return ESP_OK;
    }
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_network(wifi_creds_get(0));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());
    set_wifi_state(WIFI_ST_IDLE);

    if (xTaskCreatePinnedToCore(sync_task, "sync", 8192, NULL, 4, &s_task, 0) != pdPASS)
        sys_event(EV_ERROR, "Clock-sync task could not start (internal RAM %u B)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

#else  /* CONFIG_OCM_WIFI_CLOCK_SYNC off: no Wi-Fi stack, no clock sync */

esp_err_t wifi_sync_start(void)
{
    STATUS_LOCK(); g_status.wifi = WIFI_ST_DISABLED; STATUS_UNLOCK();
    ESP_LOGI(TAG, "Wi-Fi clock sync compiled out");
    return ESP_OK;
}

bool wifi_sync_connected(void) { return false; }
bool wifi_sync_connect_now(void) { return false; }

#endif
