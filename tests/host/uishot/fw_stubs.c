/* Firmware data for the host renderer: every producer the screens read, filled with the widest values each field can
   show (long names, many digits, negative numbers), so textfit measures the worst case. uishot_fill() sets them.
   The demo case (uishot_case.demo) publishes realistic example values instead, for the README and the manual: a car
   connected and logging at 64 km/h, R.E.V in use and learning, no faults, no network names, no MAC or VIN. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_host.h"
#include "status.h"
#include "mosaico_ui.h"
#include "mosaico_board.h"
#include "mosaico_clock.h"
#include "mosaico_drive.h"
#include "mosaico_files.h"
#include "ml_infer.h"
#include "rev_task.h"
#include "sched_mon.h"
#include "sdlog.h"
#include "sys_mode.h"
#include "uploader.h"
#include "remote_config.h"
#include "elm327.h"
#include "os.h"
#include "os_core.h"
#include "fault_engine.h"
#if CONFIG_OCM_NAND_LOGS
#include "nand_store.h"
#endif
#include "uishot.h"

app_status_t g_status;
SemaphoreHandle_t g_status_mutex;
uishot_case_t uishot_case;

/* ---- ESP-IDF ---- */
const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_FAIL"; }
void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
void *heap_caps_calloc(size_t c, size_t n, uint32_t caps) { (void)caps; return calloc(c, n); }
void *heap_caps_realloc(void *p, size_t n, uint32_t caps) { (void)caps; return realloc(p, n); }
void *heap_caps_aligned_alloc(size_t a, size_t n, uint32_t caps) { (void)caps; void *p = NULL; return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n) ? NULL : p; }
void *heap_caps_aligned_calloc(size_t a, size_t c, size_t n, uint32_t caps) { void *p = heap_caps_aligned_alloc(a, c * n, caps); if (p) memset(p, 0, c * n); return p; }
void heap_caps_free(void *p) { free(p); }
/* widest: internal 3 digits KB, PSRAM "31.9 MB" */
size_t heap_caps_get_free_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? (size_t)(31.9 * 1048576) : 999 * 1024; }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 30u << 20 : 999 * 1024; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 31744; }
size_t heap_caps_get_total_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 32u << 20 : 400 * 1024; }
static int64_t s_now_us = 3723000000LL;                  /* 1 h 2 min 3 s after boot */
int64_t esp_timer_get_time(void) { return s_now_us; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { (void)a; *h = NULL; return ESP_OK; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us) { (void)h; (void)us; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) { (void)h; (void)us; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t h) { (void)h; return ESP_OK; }
uint32_t esp_cpu_get_cycle_count(void) { return 0; }
uint32_t esp_random(void) { return 4; }
void esp_restart(void) { abort(); }
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
uint32_t esp_get_free_heap_size(void) { return 999 * 1024; }
uint32_t esp_get_minimum_free_heap_size(void) { return 999 * 1024; }
void esp_chip_info(esp_chip_info_t *ci) { memset(ci, 0, sizeof *ci); ci->revision = 999; ci->cores = 2; }
const char *esp_get_idf_version(void) { return "v6.1-dev-9999-g0123456789"; }
size_t esp_psram_get_size(void) { return 32u << 20; }
esp_flash_t *esp_flash_default_chip;
esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out) { (void)chip; *out = 16u << 20; return ESP_OK; }
esp_err_t esp_task_wdt_add(void *t) { (void)t; return ESP_OK; }
esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
esp_err_t esp_task_wdt_delete(void *t) { (void)t; return ESP_OK; }
esp_err_t esp_cache_msync(void *a, size_t n, int f) { (void)a; (void)n; (void)f; return ESP_OK; }
void vTaskDelay(TickType_t t) { (void)t; }
void vTaskDelete(TaskHandle_t t) { (void)t; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *n, uint32_t s, void *a, UBaseType_t p, TaskHandle_t *h, BaseType_t c)
{ (void)f; (void)n; (void)s; (void)a; (void)p; (void)c; if (h) *h = NULL; return pdPASS; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int d) { (void)h; (void)c; (void)d; return ESP_OK; }
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t b, const esp_lcd_panel_io_spi_config_t *c, esp_lcd_panel_io_handle_t *io) { (void)b; (void)c; *io = NULL; return ESP_OK; }
esp_err_t esp_lcd_new_panel_io_i2c(esp_lcd_i2c_bus_handle_t b, const esp_lcd_panel_io_i2c_config_t *c, esp_lcd_panel_io_handle_t *io) { (void)b; (void)c; *io = NULL; return ESP_OK; }
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io) { (void)io; return ESP_OK; }
esp_err_t esp_lcd_new_panel_co5300(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *c, esp_lcd_panel_handle_t *p) { (void)io; (void)c; *p = NULL; return ESP_OK; }
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p) { (void)p; return ESP_OK; }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) { (void)p; return ESP_OK; }
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) { (void)p; (void)on; return ESP_OK; }
esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t p, bool s) { (void)p; (void)s; return ESP_OK; }
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x1, int y1, int x2, int y2, const void *d) { (void)p; (void)x1; (void)y1; (void)x2; (void)y2; (void)d; return ESP_OK; }
esp_err_t esp_lcd_panel_co5300_set_brightness(esp_lcd_panel_handle_t p, uint8_t v) { (void)p; (void)v; return ESP_OK; }
esp_err_t esp_lcd_touch_new_i2c_cst9220(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *c, esp_lcd_touch_handle_t *t) { (void)io; (void)c; *t = NULL; return ESP_OK; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t t) { (void)t; return ESP_OK; }
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t t, esp_lcd_touch_point_data_t *d, uint8_t *n, uint8_t m) { (void)t; (void)d; (void)m; *n = 0; return ESP_OK; }
esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t t) { (void)t; return ESP_OK; }
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *c) { (void)c; return ESP_OK; }
void *lvgl_port_add_disp(const lvgl_port_display_cfg_t *c) { (void)c; return NULL; }
bool lvgl_port_lock(uint32_t ms) { (void)ms; return true; }
void lvgl_port_unlock(void) {}

/* ---- firmware producers ---- */
static void ev_put(app_status_t *st, int i, event_level_t lv, const char *t, int64_t ms)
{
    st->events[i].level = lv;
    st->events[i].uptime_ms = ms;
    snprintf(st->events[i].text, sizeof st->events[i].text, "%s", t);
}

/* the 55-character event texts (EVENT_TEXT 56): the widest letters the firmware's texts use */
static const char *const EV_LONG[] = {
    "Wi-Fi held (boot health bad): start it in Wi-Fi app",
    "Clock sync refused: www.google.com gave 1970-01-01 00:00",
    "Drive s000351 saved (80 rows), NAND 114.3 of 114.4 MB",
    "MOMMWWMMWWMMWWMMWWMMWWMMWWMMWWMMWWMMWWMMWWMMWWMMWWMMWWM",
};


/* the demo case: one ordinary moment of a drive, values that go together (64 km/h in 4th at 2350 rpm, warm engine,
   alternator charging, the tank at 46 % of a 50 L tank) */
static void demo_fill(void)
{
    app_status_t *st = &g_status;
    memset(st, 0, sizeof *st);
    st->ble = uishot_case.idle ? BLE_ST_SCANNING : BLE_ST_LOGGING;   /* idle: the same board before a car connects */
    snprintf(st->adapter_name, sizeof st->adapter_name, "%s", "OBDLink CX");
    st->adapter_saved = true;
    st->adapter_rssi = -58;
    st->sd_mounted = true;
    st->session_id = 352;
    st->rows_written = 1260;
    st->bytes_written = 418760;
    st->wifi = WIFI_ST_IDLE;
    st->time_synced = true;
    obd_values_t *o = &st->obd;
    static const struct { int f; float v; } F[] = {
        {F_load, 34}, {F_coolant, 88}, {F_stft1, 1.6f}, {F_ltft1, -2.3f}, {F_map, 48}, {F_rpm, 2350}, {F_speed, 64},
        {F_timing, 17.5f}, {F_iat, 31}, {F_maf, 9.6f}, {F_throttle, 19}, {F_run_time, 1260}, {F_dist_mil, 0},
        {F_fuel_level, 46}, {F_warmups, 12}, {F_dist_clr, 1840}, {F_baro, 101}, {F_cat_b1s1, 512}, {F_modv, 14.1f},
        {F_abs_load, 31}, {F_equiv, 1.0f}, {F_rel_throttle, 11}, {F_ambient, 14}, {F_pedal_d, 21}, {F_pedal_e, 10},
        {F_throttle_act, 19}, {F_o2_s1_v, 0.45f}, {F_o2_s2_v, 0.62f}, {F_fuel_type, 1}, {F_oil, 91},
    };
    for (size_t i = 0; i < sizeof F / sizeof F[0] && !uishot_case.idle; i++) { o->f[F[i].f] = F[i].v; OBD_SET(o, F[i].f); }
    o->adapter_voltage_v = 14.1f; o->adapter_v_valid = true;
    o->mil_on = false; o->status_valid = true;
    snprintf(o->protocol, sizeof o->protocol, "%s", "ISO 15765-4 CAN 11/500");
    o->fuel_lph_calc = 4.1f;   /* 64 km/h at 15.8 km/L = 4.05 L/h */ o->fuel_src = FUEL_LEARNED; o->trip_km = 21.4; o->trip_fuel_l = 1.35f;
    o->econ_trip_kpl = 15.8f; o->econ_inst_kpl = 15.8f;
    o->fuel_lph_raw = 3.95f; o->fuel_cal = 1.04f; o->trip_fuel_raw_l = 1.30f;
    o->torque_nm = 85; o->torque_src = 4; o->tank_left_l = 23; o->tank_range_km = 310;
    fault_engine_analyse(o->dtcs, o->pending_dtcs, o->permanent_dtcs, false, &st->faults);   /* read, no codes */
    if (uishot_case.alarm) {                    /* a real misfire code for the alarm banner, not a bench test text */
        snprintf(o->dtcs, sizeof o->dtcs, "%s", "P0300");
        o->mil_on = true; o->dtc_count = 1;
        fault_engine_analyse(o->dtcs, o->pending_dtcs, o->permanent_dtcs, true, &st->faults);
        snprintf(st->alarm_text, sizeof st->alarm_text, "%s %s", st->faults.faults[0].code, st->faults.faults[0].desc);
        st->alarm_critical = true;
        st->alarm_until_ms = INT64_MAX;
    }
    st->fault_rev = 1;
    static const char *const EV[] = {
        "Health good: Wi-Fi released",
        "Clock synced",
        "CarMind R.E.V admitted, margin 995.8 ms",
        "Car answered: Driving",
        "Drive s000352 started",
        "R.E.V learned: update kept",
    };
    int n = (int)(sizeof EV / sizeof EV[0]);
    for (int i = 0; i < n; i++) ev_put(st, i, EV_INFO, EV[i], 70000 + (int64_t)i * 600000);
    st->event_head = 0; st->event_count = n;
    st->cpu_pct[0] = 31; st->cpu_pct[1] = 18;
    st->heap_free_kb = 129; st->psram_free_kb = 24310; st->psram_total_kb = 32768;
    st->chip_temp_c = 46.5f;
    st->pmu_mips[0] = 212; st->pmu_mips[1] = 141;
    st->power_w = 1.9f;
    st->units = 0;
    st->ai_pct = 1;
    st->mips_ok = true;
}

void uishot_fill(void)
{
    if (uishot_case.demo) { demo_fill(); return; }
    app_status_t *st = &g_status;
    memset(st, 0, sizeof *st);
    st->ble = uishot_case.idle ? BLE_ST_SCANNING : BLE_ST_LOGGING;
    snprintf(st->adapter_name, sizeof st->adapter_name, "%s", "OBDII-LELink-BLE-0123456789AB");
    snprintf(st->adapter_addr, sizeof st->adapter_addr, "%s", "AA:BB:CC:DD:EE:FF");
    st->adapter_saved = true;
    st->adapter_rssi = -100;
    st->sd_mounted = true;
    st->session_id = 999999;
    st->rows_written = 9999999;
    st->bytes_written = 999999999ULL;
    st->wifi = WIFI_ST_CONNECTED;
    st->wifi_rssi = -100;
    snprintf(st->wifi_ssid, sizeof st->wifi_ssid, "%s", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    st->time_synced = true;
    obd_values_t *o = &st->obd;
    for (int f = 0; f < F_COUNT; f++) { o->f[f] = -888.8f; if (!uishot_case.idle) OBD_SET(o, f); }
    o->f[F_rpm] = 8888; o->f[F_speed] = 288; o->f[F_coolant] = -40;
    o->adapter_voltage_v = 14.88f; o->adapter_v_valid = true;
    o->mil_on = true; o->status_valid = true;
    snprintf(o->dtcs, sizeof o->dtcs, "%s", "P0301 P0302 P0303 P0304 P0171 P0174 P0335 P0340");
    snprintf(o->pending_dtcs, sizeof o->pending_dtcs, "%s", "P0420 P0430 U0100");
    snprintf(o->permanent_dtcs, sizeof o->permanent_dtcs, "%s", "P0300");
    o->dtc_count = 12;
    snprintf(o->vin, sizeof o->vin, "%s", "WWWWWWWWWWWWWWWWW");
    snprintf(o->protocol, sizeof o->protocol, "%s", "ISO 15765-4 CAN 29/500");
    o->fuel_lph_calc = 88.88f; o->fuel_src = 5; o->trip_km = 8888.8; o->trip_fuel_l = 888.8f;
    o->econ_trip_kpl = 0.06f; o->econ_inst_kpl = 0.06f;
    o->fuel_lph_raw = 88.8f; o->fuel_cal = 1.888f; o->trip_fuel_raw_l = 888.8f;
    o->torque_nm = -888.8f; o->torque_src = 4; o->tank_left_l = 188.8f; o->tank_range_km = 8888.8f;
    fault_engine_analyse(o->dtcs, o->pending_dtcs, o->permanent_dtcs, true, &st->faults);
    if (uishot_case.no_faults) memset(&st->faults, 0, sizeof st->faults);
    st->fault_rev = 1;
    if (uishot_case.alarm) {
        const fault_t *f = &st->faults.faults[0];
        snprintf(st->alarm_text, sizeof st->alarm_text, "%s %s", f->code, uishot_case.alarm_desc ? uishot_case.alarm_desc : f->desc);
        st->alarm_critical = true;
        st->alarm_until_ms = INT64_MAX;
    }
    if (uishot_case.pairing) {
        snprintf(st->sys_popup, sizeof st->sys_popup, "Desktop pairing code  %s", "888888");
        st->sys_popup_until_ms = INT64_MAX;
    }
    for (int i = 0; i < EVENT_MAX; i++)
        ev_put(st, i, (event_level_t)(i % 3), EV_LONG[i % 4], 3599000 + i * 1000);
    st->event_head = 0; st->event_count = EVENT_MAX;
    st->cpu_pct[0] = st->cpu_pct[1] = 100;
    st->heap_free_kb = 999; st->psram_free_kb = 32767; st->psram_total_kb = 32768;
    st->chip_temp_c = -40.0f;
    st->pmu_mips[0] = st->pmu_mips[1] = 320;    /* 320 MHz, at most one instruction a cycle */
    st->power_w = 8.88f;
    st->units = 0;
    st->ai_pct = 100;
    st->mips_ok = true;
}

void status_get(app_status_t *out) { *out = g_status; }
bool elm_drive_active(void) { return !uishot_case.idle; }

time_t uishot_time(time_t *t)
{
    time_t now = time(NULL);
    const char *fix = getenv("UISHOT_CLOCK");             /* "HH:MM": the worst-case screens at a chosen time */
    int h = 17, m = 31;
    if (uishot_case.demo || (fix && sscanf(fix, "%d:%d", &h, &m) == 2)) {
        struct tm tm = { .tm_year = 2026 - 1900, .tm_mon = 10, .tm_mday = 12, .tm_hour = h, .tm_min = m, .tm_sec = 20, .tm_isdst = -1 };
        if (uishot_case.demo) { tm.tm_hour = 17; tm.tm_min = 31; }
        now = mktime(&tm);
    }
    if (t) *t = now;
    return now;
}
uint32_t obd_ble_pair_seq(void) { return uishot_case.obd_pair ? 1 : 0; }
bool obd_ble_pair_offer(char *name, size_t n, const char **strength, int *secs_left)
{
    if (!uishot_case.obd_pair) return false;
    if (name && n) snprintf(name, n, "%s", uishot_case.obd_pair);
    if (strength) *strength = "strong";
    if (secs_left) *secs_left = 30;
    return true;
}

int ml_get_last_probs(float *out, int max) { static const float P0[4] = {0.25f, 0.25f, 0.25f, 0.25f}, PD[4] = {0.02f, 0.04f, 0.07f, 0.87f}; const float *P = uishot_case.demo ? PD : P0; int n = max < 4 ? max : 4; memcpy(out, P, n * sizeof *out); return n; }
void ml_get_stats(ml_stats_t *out)
{
    memset(out, 0, sizeof *out);
    out->ready = true; out->windows = 99999; out->dropped = 9999; out->failed = 9999; out->last_us = 99999; out->max_us = 99999;   /* 10 s windows: 99999 is 11 days of driving */
    out->last_class = 2; out->last_prob = 1.0f;   /* "Accelerating", the widest class */ out->last_est[0] = 188.8f; out->last_est[1] = -888.8f; out->last_est[2] = 88.88f;
    out->last_at_ms = 3600000; out->model = ml_model_active(); out->z_ok = true; out->learned_ok = true;
    out->learned_lph = 88.88f; out->prior_lph = 88.88f; out->pass2_us = 99999; out->selftest_us = 99999;
    if (uishot_case.demo) {                     /* window 126 of this drive, Cruising 87 % sure */
        out->windows = 126; out->dropped = 0; out->failed = 0; out->last_us = 4210; out->max_us = 4472;
        out->last_class = 3; out->last_prob = 0.87f; out->last_est[0] = 6.3f; out->last_est[1] = 82; out->last_est[2] = 2.2f;
        out->last_at_ms = 3719000; out->learned_lph = 4.1f; out->prior_lph = 4.4f; out->pass2_us = 1180; out->selftest_us = 2236;
    }
}
int ml_model_active(void) { return uishot_case.model; }
bool ml_model_available(int id) { (void)id; return true; }
const char *ml_model_name(int id) { return id == ML_MODEL_REV ? "CarMind R.E.V" : "CarMind E.V"; }
const char *ml_model_target(int id) { (void)id; return "esp32s31"; }
const char *ml_model_version(int id) { (void)id; return "bdcf1bb83f01"; }
int ml_selftest_state(void) { return 1; }

void rev_get_status(rev_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->enabled = out->learning = out->saved = out->sd = true;
    out->commits = out->resets = out->kept = out->failed = out->learned = out->pairs = out->dropped = out->saves = 9999999;
    out->rejected = 9999999; out->drift = 8.888f; out->mse_live = 88.88f; out->mse_prior = 88.88f;
    for (int i = 0; i < 3; i++) { out->err[i] = 888.8f; out->n_err[i] = 9999999; }
    out->k = 8.888f; out->k_ready = true; out->k_rows = 9999999; out->k_calc = true;
    out->cal = 1.888f; out->fill_rev = 99; out->used = 99; out->skipped = 99; out->fills = 99;
    out->life_l = 88888.8; out->life_km = 888888.8; out->row_ms = 8888.8f;
    out->tank_segs = 9999; out->tank_fills = 9999;
    out->torque_nm = -888.8f; out->vd_l = 8.88f; out->torque_src = 4; out->torque_ok = true;
    if (uishot_case.demo) {                     /* a few weeks in one car: 14 updates kept, three fill-ups */
        memset(out, 0, sizeof *out);
        out->enabled = out->learning = out->saved = out->sd = true;
        out->commits = 17; out->kept = 14; out->pairs = 1236; out->learned = 14; out->saves = 9;
        out->drift = 0.04f; out->mse_live = 0.03f; out->mse_prior = 0.18f;
        out->err[0] = 0.42f * 0.42f * 900; out->err[1] = 0.18f * 0.18f * 900; out->n_err[0] = out->n_err[1] = 900;
        out->k = 0.000312f; out->k_ready = true; out->k_rows = 5840; out->k_calc = true;
        out->cal = 1.04f; out->fills = 3; out->used = 3;
        out->life_l = 61.4; out->life_km = 968.2; out->row_ms = 1000;
        out->tank.have = out->tank.ready = true; out->tank.cap_raw_l = 48.1f; out->tank.span_pct = 62; out->tank.level = 46;
        out->tank.left_raw_l = 22.1f; out->tank.range_km = 310;
        out->tank_segs = 41; out->tank_fills = 3;
        out->torque_nm = 85; out->vd_l = 1.6f; out->torque_src = 4; out->torque_ok = true;
    }
}

bool mosaico_gauge_read(mosaico_gauge_t *g)
{
    memset(g, 0, sizeof *g);
    g->ok = true; g->mv = 4888; g->ma = uishot_case.idle ? 0 : -999; g->soc = 100; g->rem_mah = 65; g->full_mah = 65;
    g->design_mah = 3000; g->tte_min = 9999; g->temp_dc = -100; g->soh = 100; g->cycles = 9999;
    if (uishot_case.demo) { g->mv = 4150; g->ma = 12; g->soc = 96; g->rem_mah = 62; g->tte_min = 65535; g->temp_dc = 312; g->cycles = 3; }
    return true;
}
bool mosaico_gauge_uncalibrated(const mosaico_gauge_t *g) { return g->design_mah == 3000; }
void mosaico_storage_read(mosaico_storage_t *out)
{
    out->ok = true; out->nand = true;
    out->total_kb = (uint32_t)(114.4 * 1024); out->free_kb = (uint32_t)(114.3 * 1024);
    out->flash_total_kb = 3968; out->flash_free_kb = 3764;
    if (uishot_case.demo) out->free_kb = (uint32_t)(109.6 * 1024);
}
i2c_master_bus_handle_t mosaico_i2c_bus(void) { return NULL; }
bool mosaico_settings_load(void *blob, size_t n) { (void)blob; (void)n; return false; }
void mosaico_settings_save_request(const void *blob, size_t n) { (void)blob; (void)n; }
bool mosaico_board_request(mb_cmd_t cmd, uint32_t arg) { (void)cmd; (void)arg; return true; }
void mosaico_board_model_result(mosaico_model_result_t *out)
{
    memset(out, 0, sizeof *out);
    out->gen = 1; out->ok = false; out->model = ML_MODEL_REV;
    snprintf(out->why, sizeof out->why, "%s", "refused: a drive is running (end the drive first, then switch)");
}
bool mosaico_board_wifi_forget(const char *ssid) { (void)ssid; return true; }
void mosaico_board_wifi_join_state(mosaico_wifi_join_t *out) { memset(out, 0, sizeof *out); }
bool mosaico_board_wifi_join(const char *ssid, const char *pw) { (void)ssid; (void)pw; return true; }
void mosaico_board_wifi_list(mosaico_wifi_list_t *out)
{
    memset(out, 0, sizeof *out);
    out->gen = 1; out->n = MB_WIFI_NETS;
    for (int i = 0; i < out->n; i++) { snprintf(out->net[i].ssid, sizeof out->net[i].ssid, "%s", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"); out->net[i].rssi = -100; out->net[i].fails = 99; }
    snprintf(out->note, sizeof out->note, "%s", "Not forgotten: the Wi-Fi task is running a round, try again");
    snprintf(out->clock_note, sizeof out->clock_note, "%s", "Clock synced from www.google.com at 23:59, 1970-01-01");
    out->clock_gen = 1;
    if (uishot_case.demo) { out->n = 0; out->note[0] = 0; out->clock_note[0] = 0; }   /* no network names in a published image */
}
bool mosaico_board_wifi_scan(mosaico_wifi_scan_t *out, uint32_t have_gen)
{
    if (have_gen == 7) return false;
    memset(out, 0, sizeof *out);
    out->gen = 7; out->state = 2; out->n = WS_ROWS_MAX < 8 ? WS_ROWS_MAX : 8; out->n_seen = 99;
    for (int i = 0; i < out->n; i++) { snprintf(out->row[i].ssid, sizeof out->row[i].ssid, "%s", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"); out->row[i].rssi = -100; out->row[i].saved = i & 1; out->row[i].secure = true; }
    if (uishot_case.demo) { out->n = 0; out->n_seen = 0; }
    return true;
}
bool mosaico_board_wifi_search(void) { return true; }

bool mosaico_clock_known(const app_status_t *st) { (void)st; return !uishot_case.no_clock; }
void mosaico_clock_poll(void) {}
bool mosaico_clock_request(int y, int mo, int d, int h, int mi, const char *tz) { (void)y; (void)mo; (void)d; (void)h; (void)mi; (void)tz; return true; }
const char *mosaico_clock_result_text(mclk_result_t r) { (void)r; return "Refused: the date is before the firmware was built"; }
mclk_result_t mosaico_clock_result(void) { return (mclk_result_t)0; }
clock_src_t mosaico_clock_source(const app_status_t *st) { (void)st; return (clock_src_t)1; }

void mosaico_drive_end_request(void) {}
bool mosaico_drive_ending(void) { return false; }
void mosaico_drive_get_history(md_history_t *out)
{
    memset(out, 0, sizeof *out);
    out->gen = 1; out->n = MD_HIST;
    for (int i = 0; i < MD_HIST; i++) { out->d[i].session = 999999 - i; out->d[i].km = 888.8f; out->d[i].fuel_l = 88.8f; out->d[i].l100 = 88.8f; }
    if (uishot_case.demo) {
        static const float KM[] = {12.4f, 31.0f, 8.2f, 24.6f, 18.6f};
        static const float L100[] = {7.1f, 5.9f, 8.4f, 6.2f, 6.5f};
        out->n = (int)(sizeof KM / sizeof KM[0]) < MD_HIST ? (int)(sizeof KM / sizeof KM[0]) : MD_HIST;
        for (int i = 0; i < out->n; i++) { out->d[i].session = 347 + i; out->d[i].km = KM[i]; out->d[i].l100 = L100[i]; out->d[i].fuel_l = KM[i] * L100[i] / 100; }
    }
}
void mosaico_drive_get_summary(md_summary_t *out)
{
    memset(out, 0, sizeof *out);
    if (uishot_case.no_drive) return;           /* no drive saved yet: Home falls through to ECONOMY + WAITING */
    out->gen = 1; out->session = 999999; out->ok = true; out->start_epoch_ms = 1762905599000LL; out->start_up_ms = 3000000;
    out->dur_s = 359999; out->km = 8888.8f; out->fuel_l = 888.8f; out->l100 = 88.8f; out->avg_kph = 288.8f; out->max_kph = 288.8f;
    out->idle_pct = 100;
    for (int k = 0; k < 4; k++) out->cls_share[k] = 0.25f;
    out->brake_events = 9999; out->hard_brakes = 9999; out->n = MD_PTS; out->step_s = 1;
    for (int i = 0; i < MD_PTS; i++) { out->spd[i] = 100 + 80 * sinf(i / 9.0f); out->fuel[i] = 8 + 6 * sinf(i / 7.0f); out->cls[i] = (int8_t)(i / 60); }
    out->nev = UEV_DRIVE_EVENTS;
    for (int i = 0; i < out->nev; i++) { out->ev[i].level = (event_level_t)(i % 3); out->ev[i].uptime_ms = 3000000 + i * 1000; snprintf(out->ev[i].text, EVENT_TEXT, "%s", EV_LONG[i % 4]); }
    if (uishot_case.demo) {                     /* the last drive saved: 18.6 km in 28 min, mostly cruising */
        struct tm tm = { .tm_year = 2026 - 1900, .tm_mon = 10, .tm_mday = 12, .tm_hour = 8, .tm_min = 12, .tm_isdst = -1 };
        out->session = 351; out->start_epoch_ms = (int64_t)mktime(&tm) * 1000; out->start_up_ms = 0;
        out->dur_s = 1680; out->km = 18.6f; out->fuel_l = 1.21f; out->l100 = 6.5f; out->avg_kph = 39.9f; out->max_kph = 92;
        out->idle_pct = 12;
        static const float SH[4] = {0.12f, 0.10f, 0.21f, 0.57f};
        memcpy(out->cls_share, SH, sizeof SH);
        out->brake_events = 14; out->hard_brakes = 0;
        for (int i = 0; i < MD_PTS; i++) {
            float t = (float)i / MD_PTS;
            out->spd[i] = fmaxf(0, 45 + 30 * sinf(t * 9.0f) + 12 * sinf(t * 23.0f));
            out->fuel[i] = 1.2f + 2.5f * fmaxf(0, sinf(t * 9.0f + 0.6f));
            out->cls[i] = out->spd[i] < 3 ? 0 : (int8_t)(sinf(t * 9.0f + 0.6f) > 0.5f ? 2 : 3);
        }
        static const char *const DE[] = {"Drive s000351 started", "Car answered: Driving", "Drive s000351 saved (1680 rows)"};
        out->nev = 3;
        for (int i = 0; i < out->nev; i++) { out->ev[i].level = EV_INFO; out->ev[i].uptime_ms = 60000 + i * 800000; snprintf(out->ev[i].text, EVENT_TEXT, "%s", DE[i]); }
    }
}
void mosaico_drive_history_request(void) {}
void mosaico_drive_live(int *b, int *h) { *b = uishot_case.demo ? 9 : 999; *h = uishot_case.demo ? 0 : 999; }
void mosaico_drive_start_request(void) {}
void mosaico_drive_start(void) {}
bool mosaico_drive_take_auto_open(void) { return false; }

void mosaico_files_get_list(mf_list_t *out)
{
    memset(out, 0, sizeof *out);
    out->gen = 1; out->ok = true; out->n = MF_MAX; out->total = 9999;
    out->total_kb = (uint32_t)(114.4 * 1024); out->free_kb = (uint32_t)(114.3 * 1024); out->nand = true;
    out->flash_total_kb = 3968; out->flash_free_kb = 3764;
    snprintf(out->note, sizeof out->note, "%s", "Not deleted: s000351 is being written (end the drive first)");
    for (int i = 0; i < out->n; i++) {
        mf_session_t *s = &out->s[i];
        s->id = 999999 - i; s->bytes = 999999999; s->rows = 9999999; s->start_epoch_ms = 1762905599000LL; s->model_year = 2088;
        s->nfiles = 12; s->current = i == 0;
    }
    if (uishot_case.demo) {                     /* the drive running now and the five before it */
        out->n = 6 < MF_MAX ? 6 : MF_MAX; out->total = 6; out->free_kb = (uint32_t)(109.6 * 1024); out->note[0] = 0;
        for (int i = 0; i < out->n; i++) {
            mf_session_t *s = &out->s[i];
            s->id = 352 - i; s->rows = i ? 900 + 210 * (i % 3) : 1260; s->bytes = (uint64_t)s->rows * 332; s->model_year = 2017;
            struct tm tm = { .tm_year = 2026 - 1900, .tm_mon = 10, .tm_mday = 12 - i, .tm_hour = i ? 8 : 17, .tm_min = i ? 12 : 10, .tm_isdst = -1 };
            s->start_epoch_ms = (int64_t)mktime(&tm) * 1000; s->nfiles = 3; s->current = i == 0;
        }
    }
}
void mosaico_files_get_detail(mf_detail_t *out)
{
    memset(out, 0, sizeof *out);
    out->gen = 1; out->id = 999999; out->ok = true; out->start_epoch_ms = 1762905599000LL; out->first_up_ms = 1000; out->last_up_ms = 359999000;
    out->km = 8888.8f; out->fuel_l = 888.8f; out->rows = 9999999; out->model_year = 2088;
    snprintf(out->protocol, sizeof out->protocol, "%s", "ISO 15765-4 CAN 29/500");
    snprintf(out->adapter, sizeof out->adapter, "%s", "OBDII-LELink-BLE-0123456789AB");
    out->nf = MF_FILES;
    for (int i = 0; i < out->nf; i++) { snprintf(out->f[i].name, sizeof out->f[i].name, "s999999.%s", i & 1 ? "sum.json" : "ml.csv"); out->f[i].bytes = 999999999; }
    if (uishot_case.demo) {
        out->id = 351; out->first_up_ms = 60000; out->last_up_ms = 1740000; out->km = 18.6f; out->fuel_l = 1.21f; out->rows = 1680;
        struct tm tm = { .tm_year = 2026 - 1900, .tm_mon = 10, .tm_mday = 11, .tm_hour = 8, .tm_min = 12, .tm_isdst = -1 };
        out->model_year = 2017; out->start_epoch_ms = (int64_t)mktime(&tm) * 1000;
        snprintf(out->protocol, sizeof out->protocol, "%s", "ISO 15765-4 CAN 11/500");
        snprintf(out->adapter, sizeof out->adapter, "%s", "OBDLink CX");
        static const char *const FN[] = {"s000351.csv", "s000351.ml.csv", "s000351.sum.json"};
        static const uint64_t FB[] = {557760, 141120, 2140};
        out->nf = 3 < MF_FILES ? 3 : MF_FILES;
        for (int i = 0; i < out->nf; i++) { snprintf(out->f[i].name, sizeof out->f[i].name, "%s", FN[i]); out->f[i].bytes = FB[i]; }
    }
}

#if CONFIG_OCM_NAND_LOGS
nand_state_t nand_store_state(void) { return (nand_state_t)0; }
#endif
uint8_t os_battery_state(void) { return uishot_case.bat_state; }
os_runlevel_t os_blocker(os_svc_t svc) { (void)svc; return OS_RL_N; }
uint8_t os_health(uint16_t *flags) { if (flags) *flags = uishot_case.demo ? 0 : 0xffff; return uishot_case.health; }
const char *os_refusal(os_svc_t svc) { (void)svc; return NULL; }
os_runlevel_t os_runlevel(void) { return uishot_case.runlevel; }
uint8_t os_ui_request(void) { return 0; }
static remote_config_t s_rc = { .theme_mode = 0, .day_start_hour = 6, .day_end_hour = 17 };
const remote_config_t *remote_config_get(void) { return &s_rc; }

static const char *const JOB[] = { "ui", "ml", "rev", "obd_poll", "WWWWWWWWWWWWWWWW" };
int sched_get_stats(sched_stats_t *out, int max)
{
    int n = (int)(sizeof JOB / sizeof JOB[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].name = JOB[i]; out[i].runs = 9999999; out[i].misses = 9999; out[i].max_us = 999999; out[i].expected = true;
        out[i].period_ms = 50; out[i].deadline_ms = 50;
        if (uishot_case.demo) {
            static const uint32_t P[] = {50, 1000, 1000, 100}, MX[] = {7800, 4472, 1900, 2600};
            if (i >= 4) return 4;
            out[i].runs = 3723 * 1000 / P[i]; out[i].misses = 0; out[i].max_us = MX[i]; out[i].period_ms = out[i].deadline_ms = P[i];
        }
    }
    return n;
}
sched_job_t sched_register(const char *name, uint32_t p, uint32_t d, bool w) { (void)name; (void)p; (void)d; (void)w; return 0; }
void sched_begin(sched_job_t j) { (void)j; }
void sched_end(sched_job_t j) { (void)j; }
int64_t sdlog_ms_since_write(void) { return 999999; }
bool sdlog_ok(void) { return true; }
bool sdlog_saved(void) { return true; }
sys_mode_t sys_mode_get(void) { return (sys_mode_t)0; }
void sys_reset(sys_reset_reason_t r, const char *d) { (void)r; (void)d; abort(); }
bool uploader_busy(void) { return false; }
