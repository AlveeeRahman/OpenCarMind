#include "mosaico_board.h"
#include <string.h>
#include "board.h"
#include "board_pins_safe.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "sdlog.h"
#include "status.h"
#include <stdio.h>
#include <math.h>
#include "mosaico_files.h"
#include "obd_ble.h"
#include "elm327.h"
#include "wifi_sync.h"
#include "freertos/queue.h"
#include "mosaico_clock.h"
#include "ml_infer.h"
#if CONFIG_OCM_NAND_LOGS
#include "nand_store.h"
#endif
#include "wifi_creds.h"
#include "ui_settings_logic.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "sys_mode.h"
#include "wifi_ui_logic.h"
#include "ai_svc.h"
#include "health_view.h"
#if CONFIG_OCM_BAT_EST
#include "bat_est.h"
#endif

static const char *TAG = "mosaico";

/* BQ27220 standard commands (TI SLUUBD4A), two bytes, little endian */
#define G_VOLTAGE 0x08
#define G_CURRENT 0x0C
#define G_REMAIN  0x10
#define G_FULL    0x12
#define G_SOC     0x2C
#define G_DESIGN  0x3C
#define G_TEMP    0x06            /* Temperature(), 0.1 K */
#define G_BSTAT   0x0A            /* BatteryStatus() */
#define G_CYCLES  0x2A            /* CycleCount() */
#define G_SOH     0x2E            /* StateOfHealth(), % */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_gauge;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static mosaico_gauge_t s_last;                     /* published under s_mux, a memcpy only */
static mosaico_storage_t s_fat;
typedef struct { uint8_t cmd; uint32_t arg; } mb_req_t;
static QueueHandle_t s_q;

bool mosaico_board_request(mb_cmd_t cmd, uint32_t arg)
{
    mb_req_t r = { (uint8_t)cmd, arg };
    return s_q && xQueueSend(s_q, &r, 0) == pdTRUE;
}

static mosaico_model_result_t s_model_res;         /* published under s_mux, a memcpy only */

void mosaico_board_model_result(mosaico_model_result_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_model_res;
    portEXIT_CRITICAL(&s_mux);
}

static void model_set(int id)
{
    mosaico_model_result_t r = { .model = id };
    const char *no = os_refusal(OS_SVC_MODEL_SWITCH);   /* the runlevels (os_core.h); the door's MODEL SET says the same */
    if (no) snprintf(r.why, sizeof r.why, "%s: switch models while parked", no);
    else r.ok = ml_select_model(id, r.why, sizeof r.why);
    ESP_LOGI(TAG, "model %s from the screen: %s", ml_model_key(id), r.ok ? "loaded" : r.why);
    portENTER_CRITICAL(&s_mux);
    r.gen = s_model_res.gen + 1;
    s_model_res = r;
    portEXIT_CRITICAL(&s_mux);
}

static mosaico_wifi_list_t s_wifi;                 /* published under s_mux */
static char s_wifi_forget[33];                     /* the network a forget request names, under s_mux */
EXT_RAM_BSS_ATTR static mosaico_wifi_scan_t s_scan;   /* published under s_mux */
static mosaico_wifi_join_t s_join_st;              /* published under s_mux */
static struct { char ssid[33]; char pw[65]; bool keep; } s_join_req;   /* under s_mux; the password is wiped once read */

void mosaico_board_wifi_list(mosaico_wifi_list_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_wifi;
    portEXIT_CRITICAL(&s_mux);
}

bool mosaico_board_wifi_forget(const char *ssid)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_wifi_forget, ssid, sizeof s_wifi_forget);
    portEXIT_CRITICAL(&s_mux);
    return mosaico_board_request(MB_WIFI_FORGET, 0);
}

static void wifi_list_copy(const char *note, const char *clock_note)
{
    static mosaico_wifi_list_t w;                  /* board task only */
    memset(&w, 0, sizeof w);
    size_t n = wifi_creds_count();
    for (size_t i = 0; i < n && w.n < MB_WIFI_NETS; i++) {
        const wifi_cred_t *c = wifi_creds_get(i);
        if (!c) continue;
        strlcpy(w.net[w.n].ssid, c->ssid, sizeof w.net[0].ssid);
        w.net[w.n].rssi = c->last_rssi;
        w.net[w.n].fails = c->fail_count;
        w.n++;
    }
    portENTER_CRITICAL(&s_mux);
    w.gen = s_wifi.gen + 1;
    if (note) strlcpy(w.note, note, sizeof w.note);
    else memcpy(w.note, s_wifi.note, sizeof w.note);
    if (clock_note) strlcpy(w.clock_note, clock_note, sizeof w.clock_note);
    else memcpy(w.clock_note, s_wifi.clock_note, sizeof w.clock_note);
    w.clock_gen = s_wifi.clock_gen + (clock_note ? 1 : 0);
    s_wifi = w;
    portEXIT_CRITICAL(&s_mux);
}

static void wifi_forget(void)
{
    char ssid[33];
    portENTER_CRITICAL(&s_mux);
    memcpy(ssid, s_wifi_forget, sizeof ssid);
    portEXIT_CRITICAL(&s_mux);
    const char *no = os_refusal(OS_SVC_WIFI_EDIT);
    int r = no ? -3 : wifi_sync_forget(ssid);     /* -1: refused while a Wi-Fi round holds a pointer into the list */
    char nb[64];
    if (no) snprintf(nb, sizeof nb, "Not forgotten: %s.", no);
    const char *note = no ? nb : r > 0 ? "Network forgotten."
                     : r == 0 ? "Not forgotten: the network is no longer saved."
                     : r == -1 ? "Not forgotten: Wi-Fi is busy. Try again in a minute."
                     : "Not forgotten: the list could not be written.";
    ESP_LOGI(TAG, "Wi-Fi network forget from the screen: %s", note);     /* the name stays out of the log */
    wifi_list_copy(note, NULL);
}

static void wifi_now(void)
{
    const char *note;
    if (wifi_creds_count() == 0) note = "No saved network yet: search and join one first.";
    else if (!wifi_sync_up()) note = "Wi-Fi could not start. Restart the board and try again.";
    else if (!wifi_sync_connect_now()) note = "Wi-Fi is not available in this firmware.";
    else note = "Looking for a saved network to set the clock.";
    ESP_LOGI(TAG, "Wi-Fi clock sync now: %s", note);
    wifi_list_copy(NULL, note);
}

static void scan_publish(int state, int n, int n_seen, const ws_row_t *rows)
{
    portENTER_CRITICAL(&s_mux);
    s_scan.state = state;
    s_scan.n = n;
    s_scan.n_seen = n_seen;
    if (rows && n > 0) memcpy(s_scan.row, rows, (size_t)n * sizeof rows[0]);
    s_scan.gen++;
    portEXIT_CRITICAL(&s_mux);
}

bool mosaico_board_wifi_search(void)
{
    scan_publish(WS_ST_SEARCHING, 0, 0, NULL);
    if (mosaico_board_request(MB_WIFI_SCAN, 0)) return true;
    scan_publish(WS_ST_FAILED, 0, 0, NULL);
    return false;
}

bool mosaico_board_wifi_scan(mosaico_wifi_scan_t *out, uint32_t have_gen)
{
    portENTER_CRITICAL(&s_mux);
    bool changed = s_scan.gen != have_gen;
    if (changed) *out = s_scan;
    portEXIT_CRITICAL(&s_mux);
    return changed;
}

static bool saved_cb(const char *ssid, void *ctx) { (void)ctx; return wifi_creds_find(ssid) != NULL; }

static void ram_line(const char *when)
{
    ESP_LOGI(TAG, "Wi-Fi search %s: internal RAM %u B free, lowest %u B, largest block %u B", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

#define SCAN_RECORDS 40
static void wifi_scan(void)
{
    EXT_RAM_BSS_ATTR static wifi_seen_t seen[SCAN_RECORDS];
    EXT_RAM_BSS_ATTR static ws_raw_t raw[SCAN_RECORDS];
    EXT_RAM_BSS_ATTR static ws_row_t rows[SCAN_RECORDS + 8];
    if (!os_allows(OS_SVC_WIFI_RADIO)) { scan_publish(WS_ST_DRIVING, 0, 0, NULL); return; }
    ram_line("before");
    int64_t t0 = esp_timer_get_time();
    int n = wifi_sync_up() ? wifi_sync_scan(seen, SCAN_RECORDS) : -1;   /* not before app_main's wifi_sync_started() */
    ram_line("after");
    if (n < 0) { scan_publish(WS_ST_FAILED, 0, 0, NULL); return; }
    for (int i = 0; i < n; i++) {
        memcpy(raw[i].ssid, seen[i].ssid, sizeof raw[i].ssid);
        raw[i].ssid[sizeof raw[i].ssid - 1] = 0;
        raw[i].rssi = seen[i].rssi;
        raw[i].secure = seen[i].secure;
    }
    int n_seen = 0;
    int k = ws_merge(raw, n, rows, SCAN_RECORDS + 8, WS_ROWS_MAX, saved_cb, NULL, &n_seen);
    ESP_LOGI(TAG, "Wi-Fi search from the screen: %d networks, %d shown, %lld ms", n_seen, k,
             (long long)((esp_timer_get_time() - t0) / 1000));                     /* counts only, no names */
    scan_publish(WS_ST_DONE, k, n_seen, rows);
}

static void join_publish(int result, const char *ssid, bool bump)
{
    portENTER_CRITICAL(&s_mux);
    s_join_st.result = result;
    if (ssid) strlcpy(s_join_st.ssid, ssid, sizeof s_join_st.ssid);
    if (bump) s_join_st.gen++;
    portEXIT_CRITICAL(&s_mux);
}

/* runs in the Wi-Fi task when its join round has an answer */
static void join_done(int result, void *ctx)
{
    (void)ctx;
    join_publish(result, NULL, true);
    ESP_LOGI(TAG, "Wi-Fi join from the screen: result %d", result);
    mosaico_board_request(MB_WIFI_LIST, 0);        /* the saved list changed (or went back) */
}

bool mosaico_board_wifi_join(const char *ssid, const char *password)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_join_req.ssid, ssid, sizeof s_join_req.ssid);
    s_join_req.keep = password == NULL;
    strlcpy(s_join_req.pw, password ? password : "", sizeof s_join_req.pw);
    s_join_st.result = WJ_WAIT;
    strlcpy(s_join_st.ssid, ssid, sizeof s_join_st.ssid);
    s_join_st.gen++;
    portEXIT_CRITICAL(&s_mux);
    if (mosaico_board_request(MB_WIFI_JOIN, 0)) return true;
    portENTER_CRITICAL(&s_mux);
    ws_wipe(s_join_req.pw, sizeof s_join_req.pw);
    portEXIT_CRITICAL(&s_mux);
    join_publish(WJ_BUSY, NULL, true);
    return false;
}

void mosaico_board_wifi_join_state(mosaico_wifi_join_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_join_st;
    portEXIT_CRITICAL(&s_mux);
}

static void wifi_join(void)
{
    char ssid[33], pw[65];
    portENTER_CRITICAL(&s_mux);
    memcpy(ssid, s_join_req.ssid, sizeof ssid);
    memcpy(pw, s_join_req.pw, sizeof pw);
    bool keep = s_join_req.keep;
    ws_wipe(s_join_req.pw, sizeof s_join_req.pw);
    portEXIT_CRITICAL(&s_mux);
    int r = WJ_WAIT;
    if (!os_allows(OS_SVC_WIFI_RADIO)) r = WJ_DRIVING;
    else if (!wifi_sync_up()) r = WJ_NO_RADIO;
    else if (!wifi_sync_join(ssid, keep ? NULL : pw, join_done, NULL)) r = WJ_BUSY;
    ws_wipe(pw, sizeof pw);
    if (r != WJ_WAIT) join_publish(r, NULL, true);
}

#if CONFIG_OCM_GAUGE_CAL
/* ---------- BQ27220 calibration for the 65 mAh cell (debug command GAUGE-CAL, never run by itself) ----------
   The gauge left the factory with TI's defaults (DesignCapacity() 3000 mAh), so its state of charge and health mean
   little for the board's 65 mAh cell. This sets Design Capacity and the matching Design Energy (mAh x 3.7 V) in data
   memory: unseal with TI's default keys, full access, CONFIG UPDATE mode, write each value with the block checksum,
   leave CONFIG UPDATE with a re-init, seal. UNVERIFIED in this session (no web access): the keys 0x0414/0x3672, the
   subcommands and the data-memory addresses 0x929F / 0x92A1 are TI's documented values as recalled. So nothing is
   written unless the data-memory Design Capacity read back equals DesignCapacity() and the stored Design Energy is
   3.4 to 4.0 times it (os_bq_cal_ok): a wrong address cannot pass both. Other cell parameters (taper current, terminate
   voltage, chemistry) are not touched. */
static int read16(uint8_t cmd, bool sign, bool *ok);
#define BQ_CTRL      0x00
#define BQ_MAC_ADDR  0x3E
#define BQ_MAC_DATA  0x40
#define BQ_MAC_SUM   0x60
#define BQ_DM_DESIGN_CAP    0x929F
#define BQ_DM_DESIGN_ENERGY 0x92A1

static bool bq_ctrl(uint16_t sub)
{
    uint8_t b[3] = { BQ_CTRL, (uint8_t)sub, (uint8_t)(sub >> 8) };
    bool ok = s_gauge && i2c_master_transmit(s_gauge, b, 3, 50) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(10));
    return ok;
}

static bool bq_dm_read16(uint16_t addr, uint16_t *v)
{
    uint8_t a[3] = { BQ_MAC_ADDR, (uint8_t)addr, (uint8_t)(addr >> 8) }, r = BQ_MAC_DATA, d[2];
    if (!s_gauge || i2c_master_transmit(s_gauge, a, 3, 50) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (i2c_master_transmit_receive(s_gauge, &r, 1, d, 2, 50) != ESP_OK) return false;
    *v = (uint16_t)(d[0] << 8 | d[1]);                     /* data memory is big endian */
    return true;
}

static bool bq_dm_write16(uint16_t addr, uint16_t v)
{
    uint8_t d[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    uint8_t w[5] = { BQ_MAC_ADDR, (uint8_t)addr, (uint8_t)(addr >> 8), d[0], d[1] };
    uint8_t s[3] = { BQ_MAC_SUM, os_bq_checksum(addr, d, 2), 2 + 4 };   /* MACDataSum, then MACDataLen (data + 4) */
    if (!s_gauge || i2c_master_transmit(s_gauge, w, 5, 50) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    bool ok = i2c_master_transmit(s_gauge, s, 3, 50) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(20));
    return ok;
}

static void gauge_calibrate(uint32_t mah)
{
    bool ok = true;
    uint16_t cap = 0, energy = 0, want_e = os_bq_design_energy((uint16_t)mah, 3700);
    int std_cap = read16(G_DESIGN, false, &ok);
    const char *no = os_refusal(OS_SVC_FORMAT);            /* a maintenance action: parked (or safe mode) only */
    if (no || !ok) { ESP_LOGW(TAG, "gauge calibration refused: %s", no ? no : "gauge not read"); return; }
    ESP_LOGW(TAG, "gauge calibration: DesignCapacity() %d mAh -> %lu mAh, Design Energy -> %u mWh", std_cap, (unsigned long)mah, want_e);
    ok = bq_ctrl(0x0414) && bq_ctrl(0x3672) && bq_ctrl(0xFFFF) && bq_ctrl(0xFFFF) && bq_ctrl(0x0090);   /* unseal, full access, CFG UPDATE */
    vTaskDelay(pdMS_TO_TICKS(1100));
    ok = ok && bq_dm_read16(BQ_DM_DESIGN_CAP, &cap) && bq_dm_read16(BQ_DM_DESIGN_ENERGY, &energy);
    if (!ok || !os_bq_cal_ok(cap, (uint16_t)std_cap, energy, (uint16_t)mah)) {
        ESP_LOGE(TAG, "gauge calibration NOT written: data memory read %u mAh / %u mWh against DesignCapacity() %d (i2c %s)",
                 cap, energy, std_cap, ok ? "ok" : "failed");
        bq_ctrl(0x0092);                                    /* leave CFG UPDATE without a re-init */
        bq_ctrl(0x0030);                                    /* seal */
        return;
    }
    ok = bq_dm_write16(BQ_DM_DESIGN_CAP, (uint16_t)mah) && bq_dm_write16(BQ_DM_DESIGN_ENERGY, want_e);
    bq_ctrl(0x0091);                                        /* leave CFG UPDATE with a re-init */
    vTaskDelay(pdMS_TO_TICKS(1000));
    bq_ctrl(0x0030);
    bool rd = true;
    int now = read16(G_DESIGN, false, &rd);
    ESP_LOGW(TAG, "gauge calibration %s: DesignCapacity() now %d mAh", ok && rd && now == (int)mah ? "done" : "FAILED", now);
    sys_event(ok && rd && now == (int)mah ? EV_INFO : EV_ERROR, "Gauge set to %d mAh: %s", now, ok && rd && now == (int)mah ? "done" : "check the log");
}
#endif

static void do_request(const mb_req_t *r)
{
    switch ((mb_cmd_t)r->cmd) {
    case MB_FILES_SCAN:       mosaico_files_scan(); break;
    case MB_FILES_DETAIL:     mosaico_files_detail(r->arg); break;
    case MB_FILES_DELETE:     if (r->arg) mosaico_files_delete(r->arg); break;
    case MB_FILES_DELETE_ALL: mosaico_files_delete(0); break;
    case MB_BLE_FORGET:       obd_ble_forget_adapter(); ESP_LOGI(TAG, "adapter forgotten from the screen"); break;
    case MB_BLE_SEARCH:       elm_explore(); break;
    case MB_BLE_PAIR:         obd_ble_pair_answer(r->arg != 0); break;   /* may write NVS: this task's stack is internal */
    case MB_BLE_GUARD:        obd_ble_guard_answer(r->arg >> 1, (r->arg & 1) != 0); break;   /* may write NVS too */
    case MB_FORMAT: {
        char why[64] = "", note[64];
        uint32_t fr = 0;
        int kept = 0;
        const char *no = os_refusal(OS_SVC_FORMAT);
        if (no) snprintf(note, sizeof note, "not formatted: %s", no);
        else if (os_update_begin(), sdlog_format_storage(&fr, &kept, why, sizeof why) == ESP_OK) {
#if CONFIG_OCM_NAND_LOGS
            if (nand_store_mounted()) snprintf(note, sizeof note, "NAND drive store formatted: %lu KB free", (unsigned long)fr);
            else
#endif
            snprintf(note, sizeof note, "formatted: %lu KB free, %d state files kept", (unsigned long)fr, kept);
        } else snprintf(note, sizeof note, "not formatted: %s", why);
        if (!no) os_update_end();
        ESP_LOGI(TAG, "format storage: %s", note);
        mosaico_files_note(note);
        mosaico_files_scan();
        break;
    }
    case MB_GAUGE_CAL:
#if CONFIG_OCM_GAUGE_CAL
        gauge_calibrate(r->arg);                           /* debug builds only (Kconfig OCM_GAUGE_CAL) */
#endif
        break;
    case MB_BUZZ:
        gpio_set_level(BOARD_MOTOR, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(BOARD_MOTOR, 0);
        break;
    case MB_CLOCK_SET:        mosaico_clock_do_set(); break;
    case MB_MODEL_SET:        model_set((int)r->arg); break;
    case MB_WIFI_NOW:         wifi_now(); break;
    case MB_SETTINGS_KICK:    break;                /* wakes the loop: the settings debounce sets the next wake-up */
    case MB_WIFI_LIST:        wifi_list_copy(NULL, NULL); break;
    case MB_WIFI_FORGET:      wifi_forget(); break;
    case MB_WIFI_SCAN:        wifi_scan(); break;
    case MB_WIFI_JOIN:        wifi_join(); break;
    }
}
#define SET_MAX 64
static uint8_t s_set[SET_MAX];
static size_t s_set_n;
static ocm_debounce_t s_set_deb;                   /* under s_mux */

void mosaico_storage_read(mosaico_storage_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_fat;
    portEXIT_CRITICAL(&s_mux);
}

void mosaico_settings_save_request(const void *blob, size_t n)
{
    if (n > SET_MAX) return;
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_mux);
    memcpy(s_set, blob, n);
    s_set_n = n;
    ocm_debounce_touch(&s_set_deb, now, OCM_SAVE_QUIET_MS);
    portEXIT_CRITICAL(&s_mux);
    mosaico_board_request(MB_SETTINGS_KICK, 0);    /* a full queue only delays the write to the next 2 s round */
}

/* ms until the settings write is due (cap when none is pending) */
static int32_t settings_wait_ms(int32_t cap)
{
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_mux);
    int32_t w = ocm_debounce_wait_ms(&s_set_deb, now, OCM_SAVE_MAX_MS, cap);
    portEXIT_CRITICAL(&s_mux);
    return w;
}

bool mosaico_settings_load(void *blob, size_t n)
{
    nvs_handle_t h;
    if (nvs_open("ocm_ui", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    bool ok = nvs_get_blob(h, "set", blob, &len) == ESP_OK && len == n;
    nvs_close(h);
    return ok;
}

static void settings_flush(void)
{
    uint8_t copy[SET_MAX];
    size_t n;
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_mux);
    if (!ocm_debounce_fire(&s_set_deb, now, OCM_SAVE_MAX_MS)) { portEXIT_CRITICAL(&s_mux); return; }
    memcpy(copy, s_set, s_set_n);
    n = s_set_n;
    portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open("ocm_ui", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_set_blob(h, "set", copy, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "UI settings saved: %s", esp_err_to_name(e));
}

#if CONFIG_OCM_SWIPE_INTENT
/* smart-swipe blob: a one-slot hand-off. The UI fills the buffer only while it is free; the board task writes it to NVS
   and frees it. A save that finds the slot busy is dropped (the next one, at most 32 gestures later, carries it). */
#include "swipe_intent.h"
#define SI_MAX SI_BLOB_BYTES
static uint8_t s_si_buf[SI_MAX];                        /* internal RAM: the NVS write reads it with the cache off */
static size_t s_si_len;
static volatile bool s_si_dirty;

void mosaico_si_save_request(const void *blob, size_t n)
{
    if (n > SI_MAX || s_si_dirty) return;
    memcpy(s_si_buf, blob, n);
    s_si_len = n;
    portENTER_CRITICAL(&s_mux);                          /* publishes the copy before the flag */
    s_si_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

bool mosaico_si_load(void *blob, size_t *n)
{
    nvs_handle_t h;
    if (nvs_open("ocm_ui", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_blob(h, "si", blob, n) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void si_flush(void)
{
    if (!s_si_dirty) return;
    nvs_handle_t h;
    esp_err_t e = nvs_open("ocm_ui", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "si", s_si_buf, s_si_len);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_mux);
    s_si_dirty = false;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "smart swipe state saved (%u B): %s", (unsigned)s_si_len, esp_err_to_name(e));
}
#endif

#if CONFIG_OCM_UI_METRICS
/* Smart UI gate blob (ui_metrics.h): the same one-slot hand-off as smart swipe's, NVS "ocm_ui"/"um". PSRAM: the flash
   driver copies a source it cannot read with the cache off through its own internal buffer. */
#include "esp_attr.h"
#include "ui_metrics.h"
EXT_RAM_BSS_ATTR static uint8_t s_um_buf[UM_BLOB_MAX];
static size_t s_um_len;
static volatile bool s_um_dirty;

void mosaico_um_save_request(const void *blob, size_t n)
{
    if (n > sizeof s_um_buf || s_um_dirty) return;
    memcpy(s_um_buf, blob, n);
    s_um_len = n;
    portENTER_CRITICAL(&s_mux);
    s_um_dirty = true;
    portEXIT_CRITICAL(&s_mux);
}

bool mosaico_um_load(void *blob, size_t *n)
{
    nvs_handle_t h;
    if (nvs_open("ocm_ui", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_blob(h, "um", blob, n) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void um_flush(void)
{
    if (!s_um_dirty) return;
    nvs_handle_t h;
    esp_err_t e = nvs_open("ocm_ui", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "um", s_um_buf, s_um_len);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_mux);
    s_um_dirty = false;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "smart UI gate saved (%u B): %s", (unsigned)s_um_len, esp_err_to_name(e));
}
#endif

i2c_master_bus_handle_t mosaico_i2c_bus(void) { return s_bus; }

static int read16(uint8_t cmd, bool sign, bool *ok)
{
    uint8_t b[2];
    if (!s_gauge || i2c_master_transmit_receive(s_gauge, &cmd, 1, b, 2, 50) != ESP_OK) { *ok = false; return 0; }
    uint16_t v = (uint16_t)(b[0] | (b[1] << 8));
    return sign ? (int16_t)v : (int)v;
}

static void gauge_poll(mosaico_gauge_t *g)
{
    bool ok = true;
    g->mv = read16(G_VOLTAGE, false, &ok);
    g->ma = read16(G_CURRENT, true, &ok);
    g->soc = read16(G_SOC, false, &ok);
    g->rem_mah = read16(G_REMAIN, false, &ok);
    g->full_mah = read16(G_FULL, false, &ok);
    g->design_mah = read16(G_DESIGN, false, &ok);
    g->tte_min = read16(0x16, false, &ok);
    g->temp_dc = read16(G_TEMP, false, &ok) - 2732;  /* 0.1 K -> 0.1 C */
    g->bstat = read16(G_BSTAT, false, &ok);
    g->cycles = read16(G_CYCLES, false, &ok);
    g->soh = read16(G_SOH, false, &ok) & 0xFF;     /* the low byte is the % */
    g->ok = ok;
}

bool mosaico_gauge_read(mosaico_gauge_t *g)
{
    portENTER_CRITICAL(&s_mux);
    *g = s_last;
    portEXIT_CRITICAL(&s_mux);
    return g->ok;
}

bool mosaico_gauge_uncalibrated(const mosaico_gauge_t *g) { return g->ok && (g->design_mah >= 1000 || g->full_mah >= 1000); }

/* one-off storage report 16 s after boot: used and free space, file count and the five largest files under the mount
   point (names are session ids and state files only), so a full FAT can be explained from the console */
#include <dirent.h>
#include <sys/stat.h>
typedef struct { char name[128]; uint32_t bytes; } big_t;
static void walk(const char *dir, int depth, big_t *big, int *files, uint64_t *bytes)
{
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        char p[128];
        snprintf(p, sizeof p, "%s/%.40s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 3) walk(p, depth + 1, big, files, bytes); continue; }
        (*files)++;
        *bytes += (uint64_t)st.st_size;
        for (int i = 0; i < 5; i++) if ((uint32_t)st.st_size > big[i].bytes) {
            memmove(&big[i + 1], &big[i], (4 - i) * sizeof big[0]);
            snprintf(big[i].name, sizeof big[i].name, "%s", p + strlen(CONFIG_SDLOG_MOUNT_POINT));
            big[i].bytes = (uint32_t)st.st_size;
            break;
        }
    }
    closedir(d);
}

static void storage_report(void)
{
    if (!sdlog_ok()) { ESP_LOGW(TAG, "FAT: not mounted"); return; }
    uint64_t total = 0, free_b = 0, bytes = 0;
    esp_vfs_fat_info(CONFIG_SDLOG_MOUNT_POINT, &total, &free_b);
    static big_t big[5];
    memset(big, 0, sizeof big);
    int files = 0;
    walk(CONFIG_SDLOG_MOUNT_POINT, 0, big, &files, &bytes);
    ESP_LOGI(TAG, "FAT: %llu of %llu KB free, %d files holding %llu KB", (unsigned long long)(free_b / 1024),
             (unsigned long long)(total / 1024), files, (unsigned long long)(bytes / 1024));
    for (int i = 0; i < 5 && big[i].bytes; i++) ESP_LOGI(TAG, "FAT largest %d: %s %lu B", i + 1, big[i].name, (unsigned long)big[i].bytes);
#if CONFIG_OCM_NAND_LOGS
    ESP_LOGI(TAG, "NAND: %s, %s", nand_store_describe(), nand_store_mounted() ? "mounted" : "NOT mounted");
    if (nand_store_info(&total, &free_b)) {
        memset(big, 0, sizeof big);
        files = 0;
        bytes = 0;
        walk(CONFIG_OCM_NAND_MOUNT_POINT, 0, big, &files, &bytes);
        ESP_LOGI(TAG, "NAND FAT: %llu of %llu KB free, %d files holding %llu KB", (unsigned long long)(free_b / 1024),
                 (unsigned long long)(total / 1024), files, (unsigned long long)(bytes / 1024));
    }
#endif
}

/* period 2 s, lowest priority: the gauge changes slowly and the I2C bus is shared with the touch controller */
static void board_task(void *arg)
{
    (void)arg;
    TickType_t release = xTaskGetTickCount();
    for (int tick = 0;; tick++) {
        mosaico_gauge_t g = {0};
        gauge_poll(&g);
        portENTER_CRITICAL(&s_mux);
        s_last = g;
        portEXIT_CRITICAL(&s_mux);
        ai_svc_gauge_publish(&g);                  /* the AI services' copy of this sample (read-only, no extra I2C) */
        ai_svc_board_tick(false);                  /* their NVS saves and log rows, here where storage work belongs */
#if CONFIG_OCM_AI_SVC
        health_mon_tick();                         /* new issues: the Health app's recent list and the banner */
#endif
        settings_flush();
#if CONFIG_OCM_SWIPE_INTENT
        si_flush();
#endif
#if CONFIG_OCM_UI_METRICS
        um_flush();
#endif
        if (tick == 8) storage_report();
        if (tick % 5 == 0 && sdlog_ok()) {
            uint64_t total = 0, free_b = 0;
            mosaico_storage_t f = { .ok = esp_vfs_fat_info(CONFIG_SDLOG_MOUNT_POINT, &total, &free_b) == ESP_OK };
            f.total_kb = (uint32_t)(total / 1024); f.free_kb = (uint32_t)(free_b / 1024);
#if CONFIG_OCM_NAND_LOGS
            if (nand_store_info(&total, &free_b)) {
                f.nand = true;
                f.flash_total_kb = f.total_kb; f.flash_free_kb = f.free_kb;
                f.total_kb = (uint32_t)(total / 1024); f.free_kb = (uint32_t)(free_b / 1024);
            }
#endif
            portENTER_CRITICAL(&s_mux);
            s_fat = f;
            portEXIT_CRITICAL(&s_mux);
        }
        if (tick % 5 == 0) {                            /* the status-bar counters, once every 10 s */
            STATUS_LOCK();
            uint8_t c0 = g_status.cpu_pct[0], c1 = g_status.cpu_pct[1], ai = g_status.ai_pct;
            float mips = g_status.pmu_mips[0] + g_status.pmu_mips[1];
            bool mok = g_status.mips_ok;
            STATUS_UNLOCK();
            char pw[16];
            if (!g.ok) snprintf(pw, sizeof pw, "--");
            else if (g.ma > -5 && g.ma < 5) snprintf(pw, sizeof pw, "USB");   /* on USB the gauge sees ~0 mA */
            else snprintf(pw, sizeof pw, "%.2fW", g.mv * fabs((double)g.ma) / 1e6);
            char mb[12];
            if (mok) snprintf(mb, sizeof mb, "%.0f", (double)mips); else snprintf(mb, sizeof mb, "--");
#if CONFIG_OCM_SWIPE_INTENT
            char sw[40];
            mosaico_display_swipe_brief(sw, sizeof sw);
            printf("SYS cpu=%d c0=%d c1=%d ai=%d mips=%s pwr=%s swipe=%s\n", (c0 + c1 + 1) / 2, c0, c1, ai, mb, pw, sw);
#else
            printf("SYS cpu=%d c0=%d c1=%d ai=%d mips=%s pwr=%s\n", (c0 + c1 + 1) / 2, c0, c1, ai, mb, pw);
#endif
        }
        if (tick % 30 == 0 && g.ok)
            ESP_LOGI(TAG, "PWR soc=%d%% mv=%d ma=%d rem=%d/%d mAh design=%d temp=%.1fC soh=%d%% cycles=%d status=0x%04x%s", g.soc,
                     g.mv, g.ma, g.rem_mah, g.full_mah, g.design_mah, g.temp_dc / 10.0, g.soh, g.cycles, (unsigned)g.bstat,
                     mosaico_gauge_uncalibrated(&g) ? " (gauge not configured for the 65 mAh cell)" : "");
#if CONFIG_OCM_BAT_EST
        bat_est_out_t be;
        if (tick % 30 == 0 && g.ok && bat_est_read(&be))
            ESP_LOGI(TAG, "BAT est soc=%.1f%% soh=%.0f%% dcir=x%.2f mode=%s%s%s, %lu steps, log %lu rows (%lu dropped, %lu skipped)",
                     (double)be.soc_pct, (double)be.soh_pct, (double)be.dcir_ratio, bat_mode_name(be.mode), be.anchored ? "" : ", learning",
                     be.placeholder ? ", placeholder model" : "", (unsigned long)be.steps, (unsigned long)be.log_rows,
                     (unsigned long)be.log_dropped, (unsigned long)be.log_skipped);
#endif
        os_tick();                                 /* mossREV OS: runlevel changes in the log */
#if CONFIG_OCM_UPS
        os_power_sample(g.ok, g.ma, g.mv);         /* the UPS state (os_core.h section 6) */
#endif
#if CONFIG_OCM_BATSAFE
        os_battery_sample(g.ok, g.temp_dc, g.mv, g.ma > -5);   /* battery safety (os_core.h section 8) */
#endif
        /* sleep until the next 2 s release, serving UI requests and writing the settings when their debounce is due */
        for (;;) {
            TickType_t now = xTaskGetTickCount(), due = release + pdMS_TO_TICKS(2000);
            if ((int32_t)(due - now) <= 0) { release = due; break; }
            TickType_t wait = due - now, sw = pdMS_TO_TICKS(settings_wait_ms(2000));
            if (sw < wait) wait = sw ? sw : 1;
            mb_req_t r;
            if (xQueueReceive(s_q, &r, wait) == pdTRUE) do_request(&r);
            settings_flush();
        }
    }
}

void mosaico_board_init(void)
{
    /* the Wi-Fi driver logs "connected with <network name> ... bssid <MAC>" at INFO and its block-ack line "<ba-add> ...
       TAHI/TALO" (the access point address in hex) at WARN: its errors only, so no network name or access point address
       reaches the serial log (our own Wi-Fi lines print "<network>"). Its other WARN lines are the PHY power table, the
       BT-coexistence scan-time note and the "(trc)band" rate line, none an error. The BLE controller prints the board's
       Bluetooth MAC at INFO: its warnings only. */
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("BLE_INIT", ESP_LOG_WARN);
    /* The "lowest" internal RAM figure (Board page, Wi-Fi lines) is heap_caps_get_minimum_free_size(), the sum of each
       heap's own minimum. ESP-IDF sets the minimum of a heap added after startup to 0 (heap_caps_add_region_with_caps),
       and the 32 KB internal reserve (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL) is added that way: its minimum read 0 although
       nothing had used it, so the figure was 32 KB too low for the whole run (board: "lowest 49 KB", the reserve pool
       free 32031 B, allocated 0, min_free 0). Starting the minimum here sets every heap's minimum to its free size now,
       so the figure is the lowest since this point of the boot; nothing calls the matching stop. */
    heap_caps_monitor_local_minimum_free_size_start();
    /* Safe states first: motor off; the 3.3 V rail (VCC_3V3, GPIO60 active low) ON. The USB-C controller HUSB320 and the
       boost converter hang off it; switching it off made USB re-enumerate every few seconds (clean project, 2026-11-11). */
    gpio_hold_dis(BOARD_LCD_PWR);
    gpio_hold_dis(BOARD_MOTOR);
    gpio_set_level(BOARD_LCD_PWR, 0);
    gpio_config_t out = { .pin_bit_mask = (1ULL << BOARD_LCD_PWR) | (1ULL << BOARD_MOTOR), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(BOARD_LCD_PWR, 0);
    gpio_set_level(BOARD_MOTOR, 0);
    if (rtc_gpio_is_valid_gpio(BOARD_BUTTON)) rtc_gpio_deinit(BOARD_BUTTON);
    /* every pin the firmware does not use: disabled, no pull (held, strapping pins not), the amplifier enable low */
    esp_err_t pe = board_pins_safe_apply();
    if (pe != ESP_OK) ESP_LOGW(TAG, "safe pin state not applied: %s", esp_err_to_name(pe));
    mosaico_clock_boot();                          /* app_main has just marked a time the RTC kept as synced */

    i2c_master_bus_config_t bc = { .i2c_port = BOARD_I2C_PORT, .sda_io_num = BOARD_I2C_SDA, .scl_io_num = BOARD_I2C_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) { ESP_LOGE(TAG, "I2C0 did not start: no gauge, no touch"); return; }
    if (i2c_master_probe(s_bus, BOARD_FUEL_GAUGE_ADDR, 50) == ESP_OK) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BOARD_FUEL_GAUGE_ADDR, .scl_speed_hz = 100000 };
        i2c_master_bus_add_device(s_bus, &dc, &s_gauge);
    } else {
        ESP_LOGE(TAG, "no BQ27220 at 0x%02x: no battery readings", BOARD_FUEL_GAUGE_ADDR);
    }
    mosaico_gauge_t g = {0};
    gauge_poll(&g);
    s_last = g;
    ESP_LOGI(TAG, "ESP-Mosaico: battery soc=%d%% mv=%d ma=%d (gauge %s)", g.soc, g.mv, g.ma, g.ok ? "ok" : "not read");
    s_q = xQueueCreate(8, sizeof(mb_req_t));
    xTaskCreatePinnedToCore(board_task, "board", 6144, NULL, 1, NULL, 0);
}
