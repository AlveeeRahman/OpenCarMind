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
    case MB_WIFI_NOW:         ESP_LOGI(TAG, "Wi-Fi clock sync now: %s", wifi_sync_connect_now() ? "started" : "not available"); break;
    }
}
#define SET_MAX 64
static uint8_t s_set[SET_MAX];
static size_t s_set_n;
static volatile bool s_set_dirty;

void mosaico_storage_read(mosaico_storage_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_fat;
    portEXIT_CRITICAL(&s_mux);
}

void mosaico_settings_save_request(const void *blob, size_t n)
{
    if (n > SET_MAX) return;
    portENTER_CRITICAL(&s_mux);
    memcpy(s_set, blob, n);
    s_set_n = n;
    s_set_dirty = true;
    portEXIT_CRITICAL(&s_mux);
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
    portENTER_CRITICAL(&s_mux);
    if (!s_set_dirty) { portEXIT_CRITICAL(&s_mux); return; }
    memcpy(copy, s_set, s_set_n);
    n = s_set_n;
    s_set_dirty = false;
    portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open("ocm_ui", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_set_blob(h, "set", copy, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "UI settings saved: %s", esp_err_to_name(e));
}

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
            printf("SYS cpu=%d c0=%d c1=%d ai=%d mips=%s pwr=%s\n", (c0 + c1 + 1) / 2, c0, c1, ai, mb, pw);
        }
        if (tick % 30 == 0 && g.ok)
            ESP_LOGI(TAG, "PWR soc=%d%% mv=%d ma=%d rem=%d/%d mAh design=%d%s", g.soc, g.mv, g.ma, g.rem_mah, g.full_mah,
                     g.design_mah, mosaico_gauge_uncalibrated(&g) ? " (gauge not configured for the 65 mAh cell)" : "");
        /* sleep until the next 2 s release, serving UI requests meanwhile */
        for (;;) {
            TickType_t now = xTaskGetTickCount(), due = release + pdMS_TO_TICKS(2000);
            if ((int32_t)(due - now) <= 0) { release = due; break; }
            mb_req_t r;
            if (xQueueReceive(s_q, &r, due - now) == pdTRUE) do_request(&r);
        }
    }
}

/* Key identification (2026-11-11): logs every edge of the two keys the firmware can read, the Function key (GPIO7) and
   BOOT (GPIO61, a strap pin, read only as an input here). The POWER key goes to the SAM8108 load switch and has no
   input line. Read-only: no key action changes. */
static void key_log_task(void *arg)
{
    (void)arg;
    static const struct { gpio_num_t pin; const char *name; } K[2] = {{GPIO_NUM_7, "Function key GPIO7"}, {GPIO_NUM_61, "BOOT GPIO61"}};
    int last[2];
    for (int i = 0; i < 2; i++) {
        if (K[i].pin != BOARD_BUTTON) {
            gpio_config_t io = { .pin_bit_mask = 1ULL << K[i].pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
            gpio_config(&io);
        }
        last[i] = gpio_get_level(K[i].pin);
        ESP_LOGW("keys", "KEY %s reads %s at start", K[i].name, last[i] ? "high (released)" : "LOW (pressed?)");
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        for (int i = 0; i < 2; i++) {
            int v = gpio_get_level(K[i].pin);
            if (v != last[i]) { last[i] = v; ESP_LOGW("keys", "KEY %s %s", K[i].name, v ? "up" : "DOWN"); }
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
    xTaskCreatePinnedToCore(key_log_task, "keylog", 2560, NULL, 1, NULL, 0);
}
