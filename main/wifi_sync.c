/* Wi-Fi clock sync. The board has no battery-backed clock, so session files would carry epoch 0 until some source
 * sets the time. This module scans for a known network, connects, sets the clock (HTTPS Date header), stays connected for
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
#if BOARD_MOSAICO
#include "mosaico_clock.h"                 /* the clock's source, for the jump guard (R6) */
#include "ids_svc.h"                       /* the network guard: Wi-Fi hooks (counters only) and its verdict */
#endif
#ifndef IDS_HOOK
#define IDS_HOOK(x) ((void)0)                    /* the S3: no network guard */
#endif

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
#if BOARD_MOSAICO
        IDS_HOOK(ids_hook_wifi_disc(s_last_reason));     /* deauth and disassoc reasons are counted, nothing else */
#endif
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
#if BOARD_MOSAICO && CONFIG_OCM_IDS
    ids_hook_wifi_scan_begin();                        /* the guard's Wi-Fi head: one bitmap bit per BSSID hash */
    for (int i = 0; i < n; i++) ids_hook_wifi_ap(ap[i].bssid);
    ids_hook_wifi_scan_done();
    const char *why = NULL;
    if (ids_wifi_refuse(&why)) {                       /* the round stops here; the clock is kept */
        sys_event(EV_WARN, "Wi-Fi round stopped: network guard, %s", why);
        return NULL;
    }
#endif
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

#if BOARD_MOSAICO
static volatile uint32_t s_jump_refusals;
uint32_t wifi_sync_clock_refusals(void) { return s_jump_refusals; }
/* R6 (clock_set.h): a clock that came from Wi-Fi is not moved more than 24 h ahead or 10 min back by one answer */
static bool jump_ok(int64_t epoch)
{
    app_status_t st;
    status_get(&st);
    clock_src_t src = mosaico_clock_source(&st);
    int64_t now = (int64_t)time(NULL);
    clock_jump_t j = clock_jump_check(src, now, epoch);
    if (j == CLOCK_JUMP_OK) return true;
    long long mins = (long long)((j == CLOCK_JUMP_AHEAD ? epoch - now : now - epoch) / 60);
    s_jump_refusals = s_jump_refusals + 1;
    sys_event(EV_WARN, "Clock sync refused: %lld min %s", mins, j == CLOCK_JUMP_AHEAD ? "ahead" : "behind");
    return false;
}
#endif

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
#if BOARD_MOSAICO
    if (!jump_ok(epoch)) return false;
    const char *why = NULL;
    if (ids_wifi_refuse(&why)) { sys_event(EV_WARN, "Clock kept: network guard, %s", why); return false; }
#endif
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

#if !BOARD_MOSAICO   /* the S3 keeps the line numbers of its release image (its ESP_ERROR_CHECK texts carry them), so */
#line 168            /* it stays byte-identical: the jump guard above is the Mosaico's. Edit above this point: update 168. */
#endif
static bool driving(void) { return OCM_REFUSED(OS_SVC_WIFI_RADIO, sys_mode_get() == MODE_DRIVE); }   /* runlevels on the S31 */

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
    int8_t rssi = -127; IDS_HOOK(ids_hook_wifi_round());
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
        SYS_EVENT_NET(EV_WARN, "Wi-Fi '%s' connect failed (%u)", net->ssid, net->fail_count);
        IDS_HOOK(ids_hook_wifi_leave()); esp_wifi_disconnect();
        set_wifi_state(WIFI_ST_IDLE);
        return false;
    }
    net->fail_count = 0;
    wifi_creds_dedupe(net->ssid);

    set_wifi_state(WIFI_ST_CONNECTED);
    SYS_EVENT_NET(EV_INFO, "Wi-Fi connected: %s", net->ssid);
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
    IDS_HOOK(ids_hook_wifi_leave()); esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    set_wifi_state(WIFI_ST_IDLE);
    return ok;
}

static TaskHandle_t s_task;                        /* the sync task, woken early by wifi_sync_connect_now() */

static bool radio_get(void);                       /* the driver on demand: the S3's below, the S31's in its own block */
static void radio_put(void);

/* An S3 round: the driver is initialised for the round (and, when paired, its uploads) and de-initialised after. */
static bool sync_round(void)
{
    if (!radio_get()) return false;
    bool ok = sync_once();
    radio_put();
    return ok;
}

static void sync_task(void *arg)
{
    /* A clock kept by the RTC through a software reset or deep sleep counts as set: the first drift correction is
       then a full resync interval away instead of a scan every few seconds. */
    uint32_t wait_s = clock_is_set() && !uploader_wifi_enabled() ? CONFIG_SYNC_RESYNC_INTERVAL_H * 3600U : 0;
    if (!wait_s) vTaskDelay(pdMS_TO_TICKS(20000));   /* let the boot-time tasks take their RAM before the radio does */
    for (;;) {
        if (wait_s) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000U));   /* wifi_sync_connect_now() wakes it early */
        while (driving()) vTaskDelay(pdMS_TO_TICKS(2000));   /* never scan or connect while the car is being logged */
        bool ok = sync_round();
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

#if !BOARD_MOSAICO
/* ---- S3: the driver on demand ----
   The driver holds ~50 KB of internal RAM while initialised (static RX buffers, its task); started at boot it left as
   little as 1.7 KB free on a build with more tasks, too little for the SD card's DMA transfers and for TLS. So the
   radio is only initialised while a sync (and, when paired, its uploads) uses it, and the SD saves wait meanwhile
   (sdlog_quiet). The S31 has its own on-demand radio and boot hold in the BOARD_MOSAICO block below. */
#include "freertos/semphr.h"

#define WIFI_NEED_FREE   36000                     /* internal RAM esp_wifi_init() needs, with margin (measured on the board) */
#define WIFI_NEED_BLOCK  12000

static bool s_up;                                  /* Wi-Fi stack initialised and started */
static SemaphoreHandle_t s_power;                  /* guards s_users, wifi_up() and wifi_down() */
static int s_users;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;

static bool sync_objects(void)
{
    if (!s_ev) s_ev = xEventGroupCreate();
    if (!s_power) s_power = xSemaphoreCreateMutex();
    return s_ev && s_power;
}

/* Initialise and start the station. Called with s_power held, from radio_get() only. */
static bool wifi_up(void)
{
    static bool base_done;                       /* netif, event loop and the station interface: once per boot */
    if (s_up) return true;
    if (!base_done) {
        if (esp_netif_init() != ESP_OK) return false;
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
        esp_netif_create_default_wifi_sta();
        base_done = true;
    }
    /* A failed esp_wifi_init() does not free what it took (measured: about 5 KB per try, "Failed to deinit Wi-Fi
       driver 0x3001"), so check the room first and never retry after a failure until the next boot. */
    static bool s_init_failed;
    uint32_t free_b = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (s_init_failed || free_b < WIFI_NEED_FREE || block < WIFI_NEED_BLOCK) {
        ESP_LOGW(TAG, "Wi-Fi not started: internal RAM %lu B free, block %lu B%s", (unsigned long)free_b, (unsigned long)block,
                 s_init_failed ? ", an earlier start failed" : "");
        return false;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ie = esp_wifi_init(&cfg);
    if (ie != ESP_OK) {
        s_init_failed = true;
        sys_event(EV_WARN, "Wi-Fi off until restart: %s", esp_err_to_name(ie));   /* the event ring's text length */
        ESP_LOGW(TAG, "Wi-Fi start failed: internal RAM %lu B", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, &s_h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, &s_h_ip));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (wifi_creds_count()) apply_network(wifi_creds_get(0));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    s_up = true;                                 /* initialised: wifi_down() must undo it even if start fails */
    if (esp_wifi_start() != ESP_OK) return false;
    set_wifi_state(WIFI_ST_IDLE);
    return true;
}

/* Stop and de-initialise the driver so its internal RAM comes back. The default station netif stays: its handlers
   re-attach the driver on the next esp_wifi_start(). Called with s_power held. */
static void wifi_down(void)
{
    if (!s_up) return;
    esp_wifi_stop();
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
    esp_wifi_deinit();
    s_up = false;
    s_connected = false;
    set_wifi_state(WIFI_ST_IDLE);
    sdlog_quiet(false);
}

static bool radio_get(void)
{
    if (!sync_objects()) return false;
    xSemaphoreTake(s_power, portMAX_DELAY);
    if (!s_up) sdlog_quiet(true);                /* before the driver takes the RAM the SD transfers need */
    bool ok = wifi_up();
    if (ok) s_users++;
    else if (s_users == 0) { wifi_down(); sdlog_quiet(false); }   /* a half-started driver still holds its RAM */
    xSemaphoreGive(s_power);
    return ok;
}

static void radio_put(void)
{
    xSemaphoreTake(s_power, portMAX_DELAY);
    if (s_users > 0 && --s_users == 0) wifi_down();
    xSemaphoreGive(s_power);
}

static bool task_up(void)
{
    if (s_task) return true;
    if (xTaskCreatePinnedToCore(sync_task, "sync", 8192, NULL, 4, &s_task, 0) != pdPASS) {
        sys_event(EV_ERROR, "Clock-sync task could not start (internal RAM %u B)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    return true;
}

esp_err_t wifi_sync_start(void)
{
    if (!sync_objects()) return ESP_FAIL;
    if (wifi_creds_load() == 0) {
        ESP_LOGW(TAG, "no Wi-Fi networks known: put wifi.csv on the SD card (see docs); the clock stays unset");
        set_wifi_state(WIFI_ST_DISABLED);
        return ESP_OK;
    }
    set_wifi_state(WIFI_ST_IDLE);
    return task_up() ? ESP_OK : ESP_FAIL;       /* the radio starts only when the task syncs */
}

#else  /* BOARD_MOSAICO: app_main calls wifi_sync_start_held() instead (os_hooks.h); wifi_sync_started() starts s31_task */
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
    return ESP_OK;
}
#endif

#if BOARD_MOSAICO
/* ---- ESP-Mosaico Wi-Fi app: search and join from the screen (wifi_sync.h) ----
   The S31 runs s31_task, a copy of sync_task's loop, so a round can also be a join, and every round holds s_hold:
   pick_network() hands sync_once() a pointer into the wifi_creds list, so a forget or a join's save must not move the
   list while a round runs. sync_once() leaves the state at "scanning" when no saved network is in range; the round then
   ends idle, so the screen does not show a search that is not happening. With nothing saved a round does not scan.
   Boot hold (mossREV 2.0.0): the S31 starts nothing of Wi-Fi at boot, not even with saved networks; app_main calls
   wifi_sync_start_held() (os_hooks.h OCM_WIFI_BOOT_START) instead of wifi_sync_start(). The netif, the event handlers
   and the Wi-Fi task come up in wifi_up(): automatically once the boot health verdict lets it (os.c calls
   wifi_sync_boot_release(), os_wifi_may_start() decides), or at once when the owner searches, joins or asks for
   "Sync clock now" on the screen (wifi_sync_up()). The driver itself stays on demand (radio_get/put). */
#include "freertos/semphr.h"
#include "wifi_ui_logic.h"

#define WIFI_NEED_FREE   36000           /* internal RAM esp_wifi_init() needs, with margin (measured on the board) */
#define WIFI_NEED_BLOCK  12000

static SemaphoreHandle_t s_hold;         /* held by the Wi-Fi task for a whole round, and by a scan */
static volatile bool s_boot_done;        /* app_main has finished wifi_sync_start() */
static bool s_up_failed;                 /* a start failed: esp_wifi_init() does not give back what it took, no retry */
/* On-demand radio (the test line's design, R11b): the initialised driver holds about 40 KB of internal RAM (board:
   127.8 KB free before the first start, 79.7 KB free and 46.5 KB lowest after a join), and the car link's BLE needs that
   room. So the driver runs only while a round, a join or a screen search uses it (s_users, wradio_get/put), and is
   stopped and de-initialised after; a round never starts while driving, and one that runs ends when the drive starts.
   The station netif and the event handlers stay: the netif re-attaches the driver on the next esp_wifi_start(). */
static SemaphoreHandle_t s_power;        /* guards s_users, radio_up() and radio_down() */
static int s_users;
static bool s_up;                        /* the driver is initialised and started */
static portMUX_TYPE s_join_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool reserved;                       /* a join is queued or running */
    bool queued;                         /* not taken by the Wi-Fi task yet */
    bool keep_saved;                     /* no password given: use the saved one */
    char ssid[33];
    char pw[65];
    void (*done)(int, void *);
    void *ctx;
} s_join;                                /* under s_join_mux */

static bool join_round(bool radio);

static bool radio_up(void)
{
    if (s_up) return true;
    if (s_up_failed) return false;
    uint32_t free_b = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (free_b < WIFI_NEED_FREE || block < WIFI_NEED_BLOCK) {
        ESP_LOGW(TAG, "Wi-Fi not started: internal RAM %lu B free, block %lu B", (unsigned long)free_b, (unsigned long)block);
        return false;                    /* nothing taken yet: a later try may find the room */
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e == ESP_OK) e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (e == ESP_OK) { s_up = true; e = esp_wifi_start(); }
    if (e != ESP_OK) {
        s_up_failed = true;
        sys_event(EV_WARN, "Wi-Fi off until restart: %s", esp_err_to_name(e));
        ESP_LOGW(TAG, "Wi-Fi start failed: internal RAM %lu B", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    ESP_LOGI(TAG, "Wi-Fi driver up: internal RAM %lu B free (was %lu B), largest block %lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)free_b,
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    set_wifi_state(WIFI_ST_IDLE);
    return true;
}

static void radio_down(void)
{
    if (!s_up) return;
    uint32_t before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_wifi_stop();
    esp_wifi_deinit();
    s_up = false;
    s_connected = false;
    set_wifi_state(WIFI_ST_IDLE);
    ESP_LOGI(TAG, "Wi-Fi driver down: internal RAM %lu B free (was %lu B), largest block %lu B",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)before,
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static bool radio_get(void)
{
    if (!s_power) return false;
    xSemaphoreTake(s_power, portMAX_DELAY);
    bool ok = wradio_get(&s_users, radio_up());
    if (!ok && s_users == 0) radio_down();                     /* a half-started driver still holds its RAM */
    xSemaphoreGive(s_power);
    return ok;
}

static void radio_put(void)
{
    xSemaphoreTake(s_power, portMAX_DELAY);
    if (wradio_put(&s_users)) radio_down();
    xSemaphoreGive(s_power);
}

static void radio_idle_down(void)
{
    if (!s_power) return;
    xSemaphoreTake(s_power, portMAX_DELAY);
    if (s_users == 0) radio_down();
    xSemaphoreGive(s_power);
}

static wifi_state_t wifi_state_now(void)
{
    STATUS_LOCK(); wifi_state_t st = g_status.wifi; STATUS_UNLOCK();
    return st;
}

static bool join_queued(void)
{
    portENTER_CRITICAL(&s_join_mux);
    bool q = s_join.queued;
    portEXIT_CRITICAL(&s_join_mux);
    return q;
}

static void s31_task(void *arg)
{
    (void)arg;
    (void)sync_task;                     /* the S3's loop, not used here */
    uint32_t wait_s = clock_is_set() && !uploader_wifi_enabled() ? CONFIG_SYNC_RESYNC_INTERVAL_H * 3600U : 0;
    if (!wait_s) {                                          /* as the test line: the boot-time tasks take their RAM first */
        radio_idle_down();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20000));     /* a join or "Sync clock now" ends the wait early */
    }
    for (;;) {
        radio_idle_down();                                  /* the boot start, or a round's driver, is not kept between rounds */
        if (wait_s) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000U));   /* a join or wifi_sync_connect_now() wakes it */
        while (driving()) vTaskDelay(pdMS_TO_TICKS(2000));
        xSemaphoreTake(s_hold, portMAX_DELAY);
        bool joined = join_queued(), ok;
        bool radio = (joined || wifi_creds_count()) && !driving() && radio_get();
        if (joined) ok = join_round(radio);
        else ok = radio && wifi_creds_count() ? sync_once() : false;
        if (wifi_state_now() == WIFI_ST_SCANNING) set_wifi_state(WIFI_ST_IDLE);
        if (radio) radio_put();
        xSemaphoreGive(s_hold);
        if (uploader_wifi_enabled()) wait_s = CONFIG_SYNC_SCAN_INTERVAL_S;
        else if (ok) wait_s = CONFIG_SYNC_RESYNC_INTERVAL_H * 3600U;
        else    wait_s = clock_is_set() ? 900U : CONFIG_SYNC_SCAN_INTERVAL_S;
    }
}

static bool s31_task_up(void)
{
    if (s_task) return true;
    if (!s_hold) s_hold = xSemaphoreCreateMutex();
    if (!s_hold || xTaskCreatePinnedToCore(s31_task, "sync", 8192, NULL, 4, &s_task, 0) != pdPASS) {
        sys_event(EV_ERROR, "Clock-sync task could not start (internal RAM %u B)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    return true;
}

void wifi_sync_started(void)
{
    if (!s_hold) s_hold = xSemaphoreCreateMutex();   /* before the USB door starts: its WIFI-FORGET takes it */
    if (!s_power) s_power = xSemaphoreCreateMutex();
    s_up = s_ev != NULL;                             /* wifi_sync_start() started the driver; the task drops it when idle */
    if (s_ev) s31_task_up();                         /* a network was known at boot: wifi_sync_start() started the station */
    s_boot_done = true;
}

esp_err_t wifi_sync_start_held(void)
{
    size_t n = wifi_creds_load();
    set_wifi_state(n ? WIFI_ST_IDLE : WIFI_ST_DISABLED);
    if (n) ESP_LOGI(TAG, "Wi-Fi held until the boot health check: %u networks saved, no driver at boot", (unsigned)n);
    else ESP_LOGW(TAG, "no Wi-Fi networks known: search and join one in the Wi-Fi app; the clock stays unset");
    return ESP_OK;
}

/* owner: the screen asked (always allowed); otherwise the boot health verdict decides (os_wifi_may_start) */
static bool wifi_up(bool owner)
{
    if (!s_boot_done || s_up_failed || !s_hold || !s_power) return false;
    if (s_task) return true;
    if (!os_wifi_may_start(os_health(NULL), owner)) return false;
    if (!s_ev) {                         /* first start: netif, event loop and handlers once; the driver on demand */
        esp_err_t e = esp_netif_init();
        if (e == ESP_OK) { e = esp_event_loop_create_default(); if (e == ESP_ERR_INVALID_STATE) e = ESP_OK; }
        if (e == ESP_OK && !esp_netif_create_default_wifi_sta()) e = ESP_FAIL;
        if (e == ESP_OK) e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
        if (e == ESP_OK) e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);
        if (e == ESP_OK) { s_ev = xEventGroupCreate(); if (!s_ev) e = ESP_ERR_NO_MEM; }
        if (e != ESP_OK) {
            s_up_failed = true;
            sys_event(EV_WARN, "Wi-Fi off until restart: %s", esp_err_to_name(e));
            return false;
        }
        set_wifi_state(WIFI_ST_IDLE);
    }
    return s31_task_up();
}

bool wifi_sync_up(void) { return wifi_up(true); }

void wifi_sync_boot_release(void)
{
    if (s_task || !wifi_creds_count()) return;    /* the owner started it already, or nothing to sync with */
    uint32_t free_b = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    bool ok = wifi_up(false);
    ESP_LOGI(TAG, "Wi-Fi released after the boot health check: task %s, internal RAM %lu B free before",
             ok ? "started" : "not started", (unsigned long)free_b);
}

int wifi_sync_scan(wifi_seen_t *out, int max)
{
    if (driving() || !wifi_sync_up()) return -1;
    if (xSemaphoreTake(s_hold, pdMS_TO_TICKS(3000)) != pdTRUE) return -1;   /* a round is connecting or connected */
    if (!radio_get()) { xSemaphoreGive(s_hold); return -1; }
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = {.min = 60, .max = 120} };
    EXT_RAM_BSS_ATTR static wifi_ap_record_t ap[40];
    uint16_t n = sizeof ap / sizeof ap[0];
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e == ESP_OK) e = esp_wifi_scan_get_ap_records(&n, ap);    /* sorted by signal, the strongest n kept */
    if (e != ESP_OK) esp_wifi_clear_ap_list();
    radio_put();
    xSemaphoreGive(s_hold);
    if (e != ESP_OK) { ESP_LOGW(TAG, "app scan failed: %s", esp_err_to_name(e)); return -1; }
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (!ap[i].ssid[0]) continue;
        strlcpy(out[k].ssid, (const char *)ap[i].ssid, sizeof out[k].ssid);
        out[k].rssi = ap[i].rssi; out[k].channel = ap[i].primary; out[k].secure = ap[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    ESP_LOGI(TAG, "app scan: %d networks", k);                     /* names stay out of the log */
    return k;
}

bool wifi_sync_join(const char *ssid, const char *password, void (*done)(int result, void *ctx), void *ctx)
{
    if (!s_task) return false;
    bool ok = false;
    portENTER_CRITICAL(&s_join_mux);
    if (!s_join.reserved) {
        s_join.reserved = s_join.queued = ok = true;
        s_join.keep_saved = password == NULL;
        strlcpy(s_join.ssid, ssid, sizeof s_join.ssid);
        strlcpy(s_join.pw, password ? password : "", sizeof s_join.pw);
        s_join.done = done;
        s_join.ctx = ctx;
    }
    portEXIT_CRITICAL(&s_join_mux);
    if (ok) xTaskNotifyGive(s_task);
    return ok;
}

int wifi_sync_forget(const char *ssid)
{
    if (s_hold && xSemaphoreTake(s_hold, pdMS_TO_TICKS(200)) != pdTRUE) return -1;
    int r = !wifi_creds_find(ssid) ? 0 : wifi_creds_remove(ssid) ? 1 : -2;
    if (s_hold) xSemaphoreGive(s_hold);
    return r;
}

/* The USB door's WIFI-ADD, under the same round lock: an add or a password change never lands while a round reads the
   list (it holds a pointer into it). 1 added or updated, 0 rejected (wifi_creds_add's rules or the list is full), -1 busy. */
int wifi_sync_add(const char *ssid, const char *password)
{
    if (s_hold && xSemaphoreTake(s_hold, pdMS_TO_TICKS(200)) != pdTRUE) return -1;
    int r = wifi_creds_add(ssid, password) ? 1 : 0;
    if (s_hold) xSemaphoreGive(s_hold);
    return r;
}

/* The chosen network, connected once: 20 s for an address, the reason of a failure mapped by wj_classify(). */
static wj_result_t join_connect(wifi_cred_t *c)
{
    if (driving()) return WJ_DRIVING;
    STATUS_LOCK();
    g_status.wifi = WIFI_ST_CONNECTING;
    g_status.wifi_rssi = c->last_rssi;
    snprintf(g_status.wifi_ssid, sizeof g_status.wifi_ssid, "%s", c->ssid);
    STATUS_UNLOCK();
    apply_network(c);
    xEventGroupClearBits(s_ev, GOT_IP_BIT | LOST_BIT);
    s_last_reason = 0;
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, GOT_IP_BIT | LOST_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));
    wj_result_t r = wj_classify((b & GOT_IP_BIT) != 0, (b & LOST_BIT) ? s_last_reason : 0);
    ESP_LOGI(TAG, "join from the screen: %s (reason %d)", r == WJ_CONNECTED ? "connected" : "failed", (b & LOST_BIT) ? s_last_reason : 0);
    if (r != WJ_CONNECTED) { esp_wifi_disconnect(); set_wifi_state(WIFI_ST_IDLE); }
    return r;
}

/* A join round (s_hold held): save, connect once, tell the screen, then the clock sync as sync_once() does it. */
static bool join_round(bool radio)
{
    char ssid[33], pw[65], old_pw[65] = "";
    portENTER_CRITICAL(&s_join_mux);
    memcpy(ssid, s_join.ssid, sizeof ssid);
    memcpy(pw, s_join.pw, sizeof pw);
    bool keep = s_join.keep_saved;
    void (*done)(int, void *) = s_join.done;
    void *ctx = s_join.ctx;
    ws_wipe(s_join.pw, sizeof s_join.pw);
    s_join.queued = false;
    portEXIT_CRITICAL(&s_join_mux);

    wifi_cred_t *c = wifi_creds_find(ssid);
    bool existed = c != NULL;
    if (existed) strlcpy(old_pw, c->password, sizeof old_pw);
    wj_result_t r = WJ_WAIT;
    if (!radio) r = WJ_NO_RADIO;
    else if (keep && !existed) r = WJ_INVALID;
    else if (!keep && !existed && wifi_creds_count() >= WIFI_CREDS_MAX) r = WJ_FULL;
    else if (!keep && !harden_wifi_cred_ok(ssid, pw)) r = WJ_INVALID;
    else if (!keep && !wifi_creds_add(ssid, pw)) r = WJ_SAVE_FAILED;
    else if (!(c = wifi_creds_find(ssid))) r = WJ_SAVE_FAILED;
    bool clock_ok = false;
    if (r == WJ_WAIT) {
        r = join_connect(c);
        if (wj_forget_after(r) && !keep) {               /* a wrong password does not stay saved */
            if (existed) wifi_creds_add(ssid, old_pw);
            else wifi_creds_remove(ssid);
        }
    }
    ws_wipe(pw, sizeof pw);
    ws_wipe(old_pw, sizeof old_pw);
    if (done) done((int)r, ctx);
    portENTER_CRITICAL(&s_join_mux);
    s_join.reserved = false;
    portEXIT_CRITICAL(&s_join_mux);
    if (r != WJ_CONNECTED) return false;

    c = wifi_creds_find(ssid);
    if (c) { c->fail_count = 0; c->retry_after_ms = 0; }
    set_wifi_state(WIFI_ST_CONNECTED);
    sys_event(EV_INFO, "Wi-Fi joined from the screen");
    {
        wifi_ap_record_t ap = {0};
        char cc[3] = "";
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { cc[0] = ap.country.cc[0]; cc[1] = ap.country.cc[1]; cc[2] = 0; }
        units_detect(cc, remote_config_get()->timezone);
    }
    for (int attempt = 0; attempt < 3 && !clock_ok && s_connected && !driving(); attempt++) clock_ok = sync_clock();
    if (clock_ok) stay_connected();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    set_wifi_state(WIFI_ST_IDLE);
    return clock_ok;
}
#endif  /* BOARD_MOSAICO */

#else  /* CONFIG_OCM_WIFI_CLOCK_SYNC off: no Wi-Fi stack, no clock sync */

esp_err_t wifi_sync_start(void)
{
    STATUS_LOCK(); g_status.wifi = WIFI_ST_DISABLED; STATUS_UNLOCK();
    ESP_LOGI(TAG, "Wi-Fi clock sync compiled out");
    return ESP_OK;
}

bool wifi_sync_connected(void) { return false; }
bool wifi_sync_connect_now(void) { return false; }
#if BOARD_MOSAICO
esp_err_t wifi_sync_start_held(void) { return wifi_sync_start(); }
void wifi_sync_started(void) {}
bool wifi_sync_up(void) { return false; }
void wifi_sync_boot_release(void) {}
int  wifi_sync_scan(wifi_seen_t *out, int max) { (void)out; (void)max; return -1; }
bool wifi_sync_join(const char *ssid, const char *password, void (*done)(int result, void *ctx), void *ctx)
{
    (void)ssid; (void)password; (void)done; (void)ctx;
    return false;
}
int  wifi_sync_forget(const char *ssid) { return wifi_creds_remove(ssid) ? 1 : 0; }
#endif

#endif
