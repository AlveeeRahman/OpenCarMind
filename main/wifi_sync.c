/* Wi-Fi clock sync. The board has no battery-backed clock, so session files would carry epoch 0 until some source
 * sets the time. This module scans for a known network, connects, sets the clock over SNTP, stays connected for
 * CONFIG_SYNC_STAY_CONNECTED_S seconds and then disconnects so the radio stops competing with BLE. It scans again
 * every CONFIG_SYNC_SCAN_INTERVAL_S seconds while the clock has never been set, and every
 * CONFIG_SYNC_RESYNC_INTERVAL_H hours afterwards to correct drift. It never scans or connects while the car is being
 * logged (sys_mode DRIVE), and drops an open connection when the drive starts. Nothing but SNTP time queries leaves the board. */
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
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "wifi_sync.h"
#include "sdlog.h"
#include "esp_heap_caps.h"
#include "status.h"
#include "esp_attr.h"
#include "wifi_creds.h"
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
    if (best) ESP_LOGI(TAG, "scan: %u networks, best known '%s' rssi %d", n, best->ssid, best_rssi);
    else {
        ESP_LOGI(TAG, "scan: %u networks, none of the %u known ones in range", n, (unsigned)wifi_creds_count());
        static int listed;
        if (listed++ % 10 == 0)      /* every 10th scan, list what is visible so a wrong SSID is easy to spot */
            for (int i = 0; i < n && i < 12; i++) ESP_LOGI(TAG, "   visible: '%s' rssi %d ch %u", (const char *)ap[i].ssid, ap[i].rssi, ap[i].primary);
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
    wc.sta.scan_method = WIFI_FAST_SCAN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static bool clock_is_set(void)
{
    STATUS_LOCK(); bool ok = g_status.time_synced; STATUS_UNLOCK();
    return ok;
}

static bool sync_clock(void)
{
    bool ok = false;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t epoch_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        int64_t up_ms = esp_timer_get_time() / 1000;
        sdlog_meta_set_time_sync(epoch_ms, up_ms);
        STATUS_LOCK(); g_status.time_synced = true; STATUS_UNLOCK();
        ESP_LOGI(TAG, "clock synced, epoch %lld", (long long)tv.tv_sec);
        sys_event(EV_INFO, "Clock synced over Wi-Fi");
        ok = true;
    } else sys_event(EV_WARN, "Clock sync (SNTP) timed out");
    esp_netif_sntp_deinit();
    return ok;
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

    ESP_LOGI(TAG, "connecting to '%s' (rssi %d)", net->ssid, rssi);
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
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok && s_connected && !driving(); attempt++) ok = sync_clock();
    if (ok) stay_connected();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    set_wifi_state(WIFI_ST_IDLE);
    return ok;
}

static void sync_task(void *arg)
{
    /* A clock kept by the RTC through a software reset or deep sleep counts as set: the first drift correction is
       then a full resync interval away instead of a scan every few seconds. */
    uint32_t wait_s = clock_is_set() ? CONFIG_SYNC_RESYNC_INTERVAL_H * 3600U : 0;
    for (;;) {
        if (wait_s) vTaskDelay(pdMS_TO_TICKS(wait_s * 1000U));
        while (driving()) vTaskDelay(pdMS_TO_TICKS(2000));   /* never scan or connect while the car is being logged */
        bool ok = sync_once();
        if (ok) wait_s = CONFIG_SYNC_RESYNC_INTERVAL_H * 3600U;
        else    wait_s = clock_is_set() ? 900U : CONFIG_SYNC_SCAN_INTERVAL_S;   /* due for drift, network not in reach: ask again in 15 min */
    }
}

bool wifi_sync_connected(void) { return s_connected; }

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

    if (xTaskCreatePinnedToCore(sync_task, "sync", 8192, NULL, 4, NULL, 0) != pdPASS)
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

#endif
