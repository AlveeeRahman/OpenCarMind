#include "mosaico_board.h"
#include <string.h>
#include "board.h"
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
#include "wifi_creds.h"
#include "ui_settings_logic.h"
#include "esp_timer.h"

static const char *TAG = "mosaico";

/* BQ27220 standard commands (TI SLUUBD4A), two bytes, little endian */
#define G_VOLTAGE 0x08
#define G_CURRENT 0x0C
#define G_REMAIN  0x10
#define G_FULL    0x12
#define G_SOC     0x2C
#define G_DESIGN  0x3C

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
    if (elm_drive_active()) snprintf(r.why, sizeof r.why, "a drive is running: switch models while parked");   /* usb_door.c */
    else r.ok = ml_select_model(id, r.why, sizeof r.why);
    ESP_LOGI(TAG, "model %s from the screen: %s", ml_model_key(id), r.ok ? "loaded" : r.why);
    portENTER_CRITICAL(&s_mux);
    r.gen = s_model_res.gen + 1;
    s_model_res = r;
    portEXIT_CRITICAL(&s_mux);
}

static mosaico_wifi_list_t s_wifi;                 /* published under s_mux */
static char s_wifi_forget[33];                     /* the network a forget request names, under s_mux */

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

static void wifi_list_copy(const char *note)
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
    s_wifi = w;
    portEXIT_CRITICAL(&s_mux);
}

static void wifi_forget(void)
{
    char ssid[33];
    portENTER_CRITICAL(&s_mux);
    memcpy(ssid, s_wifi_forget, sizeof ssid);
    portEXIT_CRITICAL(&s_mux);
    STATUS_LOCK();
    wifi_state_t st = g_status.wifi;
    STATUS_UNLOCK();
    const char *note;
    if (st == WIFI_ST_SCANNING || st == WIFI_ST_CONNECTING || st == WIFI_ST_CONNECTED) note = "Not forgotten: Wi-Fi is busy. Try again when it is idle.";
    else if (wifi_creds_remove(ssid)) note = "Network forgotten.";
    else note = "Not forgotten: the network is no longer saved.";
    ESP_LOGI(TAG, "Wi-Fi network forget from the screen: %s", note);     /* the name stays out of the log */
    wifi_list_copy(note);
}

static void do_request(const mb_req_t *r)
{
    switch ((mb_cmd_t)r->cmd) {
    case MB_FILES_SCAN:       mosaico_files_scan(); break;
    case MB_FILES_DETAIL:     mosaico_files_detail(r->arg); break;
    case MB_FILES_DELETE:     if (r->arg) mosaico_files_delete(r->arg); break;
    case MB_FILES_DELETE_ALL: mosaico_files_delete(0); break;
    case MB_BLE_FORGET:       obd_ble_forget_adapter(); ESP_LOGI(TAG, "adapter forgotten from the screen"); break;
    case MB_BLE_SEARCH:       elm_explore(); break;
    case MB_FORMAT: {
        char why[64] = "", note[64];
        uint32_t fr = 0;
        int kept = 0;
        if (elm_drive_active()) snprintf(note, sizeof note, "not formatted: a drive is active");
        else if (sdlog_format_storage(&fr, &kept, why, sizeof why) == ESP_OK)
            snprintf(note, sizeof note, "formatted: %lu KB free, %d state files kept", (unsigned long)fr, kept);
        else snprintf(note, sizeof note, "not formatted: %s", why);
        ESP_LOGI(TAG, "format storage: %s", note);
        mosaico_files_note(note);
        mosaico_files_scan();
        break;
    }
    case MB_BUZZ:
        gpio_set_level(BOARD_MOTOR, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(BOARD_MOTOR, 0);
        break;
    case MB_CLOCK_SET:        mosaico_clock_do_set(); break;
    case MB_MODEL_SET:        model_set((int)r->arg); break;
    case MB_WIFI_NOW:         ESP_LOGI(TAG, "Wi-Fi clock sync now: %s", wifi_sync_connect_now() ? "started" : "not available"); break;
    case MB_SETTINGS_KICK:    break;                /* wakes the loop: the settings debounce sets the next wake-up */
    case MB_WIFI_LIST:        wifi_list_copy(NULL); break;
    case MB_WIFI_FORGET:      wifi_forget(); break;
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
        settings_flush();
#if CONFIG_OCM_SWIPE_INTENT
        si_flush();
#endif
        if (tick == 8) storage_report();
        if (tick % 5 == 0 && sdlog_ok()) {
            uint64_t total = 0, free_b = 0;
            mosaico_storage_t f = { .ok = esp_vfs_fat_info(CONFIG_SDLOG_MOUNT_POINT, &total, &free_b) == ESP_OK };
            f.total_kb = (uint32_t)(total / 1024); f.free_kb = (uint32_t)(free_b / 1024);
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
            ESP_LOGI(TAG, "PWR soc=%d%% mv=%d ma=%d rem=%d/%d mAh design=%d%s", g.soc, g.mv, g.ma, g.rem_mah, g.full_mah,
                     g.design_mah, mosaico_gauge_uncalibrated(&g) ? " (gauge not configured for the 65 mAh cell)" : "");
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
