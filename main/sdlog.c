#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "board.h"
#include "sdlog.h"
#include "version.h"
#include "esp_attr.h"
#include "events.h"
#include "harden.h"
#include "econ.h"
#include "units.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static const char *TAG = "sdlog";

static sdmmc_card_t *s_card;
static bool     s_ok;
static uint32_t s_session;
static bool     s_open;           /* a session is open for rows; false after sdlog_end_drive() until the next drive */
static FILE    *s_csv;
static char     s_csv_path[96];
static char     s_json_path[96];
static uint32_t s_rows;
static uint64_t s_bytes;          /* bytes written */
static uint64_t s_synced;         /* bytes known to be on the card */
static SemaphoreHandle_t s_lock;
static cJSON   *s_meta;
static uint32_t s_meta_rev;

#define OBD_NAME(id, csv, ...) csv ","
static const char CSV_HEADER[] =
    "seq,uptime_ms,epoch_ms," OBD_FIELDS(OBD_NAME)
    "adapter_voltage_v,mil_on,dtc_count,dtcs,pending_dtcs,permanent_dtcs,"
    "custom_1,custom_2,custom_3,custom_4,custom_5,custom_6,custom_7,custom_8,"
    "fuel_lph_calc,fuel_src,trip_km,trip_fuel_l,window_seq\n";
#undef OBD_NAME
static FILE    *s_raw;
static char     s_raw_path[96];
static uint64_t s_raw_bytes, s_raw_synced;
static FILE    *s_pat;
static char     s_pat_path[96];
static uint64_t s_pat_bytes, s_pat_synced;
static FILE    *s_ml;                         /* on-board model outputs, one line per window (sNNNNNN.ml) */
static char     s_ml_path[96];
static uint64_t s_ml_bytes, s_ml_synced;
static uint32_t s_window_seq;
static int64_t  s_last_sync_ms;        /* uptime of the last full flush to the card */
static volatile bool s_unsaved;        /* something was written since the last flush */
static volatile int64_t s_last_write_ms = -1;   /* uptime of the last write to a session file */
static WORD_ALIGNED_ATTR char s_csv_iobuf[2048];   /* internal DRAM, DMA-capable on the S3 */

/* ---------- helpers ---------- */
/* The board has no battery-backed clock. Every save writes the time to the card, so after a power cut the clock resumes
   from the last save (behind by however long the car was off) until the next Wi-Fi sync corrects it. */
#define CLOCK_PATH SDLOG_DIR "/clock.txt"
static const char *s_clock_source = "unset";   /* unset, rtc (kept through a reset), resumed (from the card), sntp */

static void save_clock_locked(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (!harden_epoch_plausible(tv.tv_sec)) return;
    FILE *f = fopen(CLOCK_PATH, "w");
    if (!f) return;
    fprintf(f, "%lld\n", (long long)tv.tv_sec);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void restore_clock(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    if (harden_epoch_plausible(now.tv_sec)) { s_clock_source = "rtc"; return; }
    FILE *f = fopen(CLOCK_PATH, "r");
    if (!f) return;
    long long saved = 0;
    int n = fscanf(f, "%lld", &saved);
    fclose(f);
    if (n != 1 || !harden_epoch_plausible(saved)) return;
    struct timeval tv = { .tv_sec = (time_t)saved, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_clock_source = "resumed";
    STATUS_LOCK(); g_status.clock_estimated = true; STATUS_UNLOCK();
    sys_event(EV_INFO, "Clock resumed from the card, Wi-Fi will correct it");
}

static void set_meta_string(cJSON *obj, const char *key, const char *value)
{
    if (cJSON_GetObjectItem(obj, key)) cJSON_ReplaceItemInObject(obj, key, cJSON_CreateString(value));
    else cJSON_AddStringToObject(obj, key, value);
}

static int64_t now_epoch_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1600000000) return 0;   /* clock not set */
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static uint32_t next_session_id(void)
{
    nvs_handle_t h;
    uint32_t id = 0;
    if (nvs_open("obdlog", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "session", &id);
        id++;
        nvs_set_u32(h, "session", id);
        nvs_commit(h);
        nvs_close(h);
    }
    return id;
}

static void meta_flush_locked(void)
{
    if (!s_meta || !s_ok) return;
    cJSON_ReplaceItemInObject(s_meta, "rows", cJSON_CreateNumber(s_rows));
    cJSON_ReplaceItemInObject(s_meta, "meta_rev", cJSON_CreateNumber(++s_meta_rev));
    set_meta_string(s_meta, "clock", s_clock_source);
    char *txt = cJSON_PrintUnformatted(s_meta);
    if (!txt) return;
    char tmp[104];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_json_path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(txt, f);
        fputc('\n', f);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        remove(s_json_path);
        rename(tmp, s_json_path);
    }
    cJSON_free(txt);
}

/* Flush every open session file and the JSON to the card. Caller holds s_lock. */
static void sync_all_locked(void)
{
    if (s_csv) { fflush(s_csv); fsync(fileno(s_csv)); s_synced = s_bytes; }
    if (s_raw) { fflush(s_raw); fsync(fileno(s_raw)); s_raw_synced = s_raw_bytes; }
    if (s_pat) { fflush(s_pat); fsync(fileno(s_pat)); s_pat_synced = s_pat_bytes; }
    if (s_ml)  { fflush(s_ml);  fsync(fileno(s_ml));  s_ml_synced = s_ml_bytes; }
    meta_flush_locked();                       /* keeps the JSON row count current */
    save_clock_locked();
    s_unsaved = false;
    s_last_sync_ms = esp_timer_get_time() / 1000;
}

void sdlog_sync_now(void)
{
    if (!s_ok || !s_csv) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sync_all_locked();
    xSemaphoreGive(s_lock);
    sys_event(EV_INFO, "Safe to turn off");
}

/* Saves on schedule even when no rows arrive, e.g. the car link stalls. Its own task, because the save (file flushes, JSON)
   needs far more stack than the FreeRTOS timer task has; the stack is in PSRAM so it costs no internal RAM, and the task
   only touches the SD card, never the flash cache. */
static void sync_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SDLOG_SYNC_PERIOD_S * 1000));
        if (s_ok && esp_timer_get_time() / 1000 - s_last_sync_ms >= (int64_t)CONFIG_SDLOG_SYNC_PERIOD_S * 1000 - 500) sdlog_sync_now();
    }
}

/* Opens a new session: CSV with its header and a fresh JSON. Caller holds s_lock. */
static esp_err_t open_session_locked(void)
{
    s_session = next_session_id();
    snprintf(s_csv_path, sizeof s_csv_path, SDLOG_SESSIONS_DIR "/s%06lu.csv", (unsigned long)s_session);
    snprintf(s_json_path, sizeof s_json_path, SDLOG_SESSIONS_DIR "/s%06lu.json", (unsigned long)s_session);

    s_csv = fopen(s_csv_path, "w");
    if (!s_csv) {
        ESP_LOGE(TAG, "cannot create %s (errno %d)", s_csv_path, errno);
        return ESP_FAIL;
    }
    /* Larger than the header and any row, so stdio always copies into internal RAM before FatFs. Without it the
       header (a flash string) reached the card straight from flash: the SDMMC driver treats a 4-byte-aligned
       non-PSRAM pointer as DMA-safe, DMA cannot read flash, and two whole sectors were written as zeros. */
    setvbuf(s_csv, s_csv_iobuf, _IOFBF, sizeof s_csv_iobuf);
    fputs(CSV_HEADER, s_csv);
    fflush(s_csv);
    fsync(fileno(s_csv));
    s_rows = 0;
    s_bytes = s_synced = strlen(CSV_HEADER);
    s_raw_bytes = s_raw_synced = s_pat_bytes = s_pat_synced = s_ml_bytes = s_ml_synced = 0;
    s_last_sync_ms = esp_timer_get_time() / 1000;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    char macs[18];
    snprintf(macs, sizeof macs, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (s_meta) cJSON_Delete(s_meta);
    s_meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(s_meta, "schema_version", 1);
    cJSON_AddNumberToObject(s_meta, "session_id", s_session);
    cJSON_AddStringToObject(s_meta, "device_mac", macs);
    cJSON_AddStringToObject(s_meta, "firmware", CARMIND_FW_STRING);
    cJSON_AddItemToObject(s_meta, "adapter", cJSON_CreateObject());
    cJSON *veh = cJSON_AddObjectToObject(s_meta, "vehicle");
    cJSON_AddItemToObject(veh, "supported_pids", cJSON_CreateObject());
    cJSON *ts = cJSON_AddObjectToObject(s_meta, "time_sync");
    cJSON_AddNumberToObject(ts, "epoch_ms", 0);
    cJSON_AddNumberToObject(ts, "uptime_ms", 0);
    cJSON_AddNumberToObject(s_meta, "started_epoch_ms", 0);
    cJSON_AddNumberToObject(s_meta, "rows", 0);
    cJSON_AddNumberToObject(s_meta, "meta_rev", 0);
    cJSON_AddItemToObject(s_meta, "dtc_history", cJSON_CreateArray());
    cJSON_AddStringToObject(s_meta, "units", units_name(units_current()));   /* how the board showed this drive */
    meta_flush_locked();

    s_open = true;
    s_unsaved = false;
    STATUS_LOCK();
    g_status.session_id = s_session;
    g_status.rows_written = 0;
    g_status.bytes_written = s_bytes;
    STATUS_UNLOCK();
    ESP_LOGI(TAG, "session %lu -> %s", (unsigned long)s_session, s_csv_path);
    return ESP_OK;
}

/* ---------- public ---------- */
esp_err_t sdlog_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_SDLOG_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0  = BOARD_SD_D0;
    slot.d1  = BOARD_SD_D1;
    slot.d2  = BOARD_SD_D2;
    slot.d3  = BOARD_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(CONFIG_SDLOG_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        sys_event(EV_ERROR, "SD card mount failed: %s", esp_err_to_name(err));
        STATUS_LOCK();
        g_status.sd_mounted = false;
        snprintf(g_status.sd_error, sizeof g_status.sd_error, "%s", esp_err_to_name(err));
        STATUS_UNLOCK();
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);

    mkdir(SDLOG_DIR, 0775);
    mkdir(SDLOG_SESSIONS_DIR, 0775);
    restore_clock();                  /* before the session opens, so its start time is not 1970 */

    /* Remove sessions from earlier boots that never logged a row (header-only CSV, no raw file). */
    {
        DIR *d = opendir(SDLOG_SESSIONS_DIR);
        struct dirent *e;
        int pruned = 0;
        while (d && (e = readdir(d)) != NULL) {
            unsigned long id;
            if (strlen(e->d_name) != 11 || e->d_name[0] != 's' || strcmp(e->d_name + 7, ".csv") != 0 ||
                sscanf(e->d_name + 1, "%06lu", &id) != 1) continue;
            char path[96];
            struct stat st;
            snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%.16s", e->d_name);
            if (stat(path, &st) != 0 || st.st_size > (off_t)strlen(CSV_HEADER)) continue;
            snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/s%06lu.raw", id);
            if (stat(path, &st) == 0) continue;
            remove(path);
            snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/s%06lu.csv", id);  remove(path);
            snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/s%06lu.json", id); remove(path);
            snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/s%06lu.pat", id);  remove(path);
            snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/s%06lu.ml.csv", id); remove(path);
            pruned++;
        }
        if (d) closedir(d);
        if (pruned) ESP_LOGI(TAG, "pruned %d empty session(s)", pruned);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t oerr = open_session_locked();
    xSemaphoreGive(s_lock);
    if (oerr != ESP_OK) return oerr;

    s_ok = true;
    STATUS_LOCK();
    g_status.sd_mounted = true;
    g_status.sd_error[0] = 0;
    STATUS_UNLOCK();
    xTaskCreateWithCaps(sync_task, "sdsync", 6144, NULL, 2, NULL, MALLOC_CAP_SPIRAM);
    return ESP_OK;
}

bool sdlog_ok(void) { return s_ok; }

void sdlog_end_drive(void)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open || s_rows == 0) { xSemaphoreGive(s_lock); return; }     /* nothing logged yet: keep the session for this drive */
    if (cJSON_HasObjectItem(s_meta, "ended_uptime_ms"))
        cJSON_ReplaceItemInObject(s_meta, "ended_uptime_ms", cJSON_CreateNumber((double)(esp_timer_get_time() / 1000)));
    else cJSON_AddNumberToObject(s_meta, "ended_uptime_ms", (double)(esp_timer_get_time() / 1000));
    sync_all_locked();
    if (s_csv) { fclose(s_csv); s_csv = NULL; }
    if (s_raw) { fclose(s_raw); s_raw = NULL; }
    if (s_pat) { fclose(s_pat); s_pat = NULL; }
    if (s_ml)  { fclose(s_ml);  s_ml = NULL; }
    s_open = false;
    uint32_t id = s_session, rows = s_rows;
    xSemaphoreGive(s_lock);
    sys_event(EV_INFO, "Drive s%06lu saved (%lu rows)", (unsigned long)id, (unsigned long)rows);
}

void sdlog_begin_drive(void)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open) open_session_locked();
    xSemaphoreGive(s_lock);
}

bool sdlog_saved(void) { return s_ok && !s_unsaved; }

int64_t sdlog_ms_since_write(void)
{
    int64_t t = s_last_write_ms;
    return t < 0 ? INT64_MAX : esp_timer_get_time() / 1000 - t;
}
uint32_t sdlog_session_id(void) { return s_session; }

static int fmt_num(char *dst, size_t n, float v)
{
    if (v == (float)(long)v && v > -1e7f && v < 1e7f) return snprintf(dst, n, "%ld,", (long)v);
    return snprintf(dst, n, "%.3f,", v);
}

void sdlog_write_row(const obd_values_t *v)
{
    if (!s_ok || !s_csv) return;
    EXT_RAM_BSS_ATTR static char line[1400];
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open || !s_csv) { xSemaphoreGive(s_lock); return; }   /* the drive was closed meanwhile */
    s_rows++;
    n += snprintf(line + n, sizeof line - n, "%lu,%lld,%lld,", (unsigned long)s_rows,
                  (long long)(esp_timer_get_time() / 1000), (long long)now_epoch_ms());
    for (int i = 0; i < F_COUNT && n < (int)sizeof line - 32; i++) {
        if (OBD_HAS(v, i)) n += fmt_num(line + n, sizeof line - n, v->f[i]);
        else line[n++] = ',';
    }
    if (v->adapter_v_valid) n += snprintf(line + n, sizeof line - n, "%.1f,", v->adapter_voltage_v);
    else line[n++] = ',';
    if (v->status_valid) n += snprintf(line + n, sizeof line - n, "%d,%u,", v->mil_on ? 1 : 0, v->dtc_count);
    else n += snprintf(line + n, sizeof line - n, ",,");
    n += snprintf(line + n, sizeof line - n, "%s,%s,%s,", v->dtcs, v->pending_dtcs, v->permanent_dtcs);
    for (int i = 0; i < 8 && n < (int)sizeof line - 16; i++) {
        if (v->custom_valid & (1 << i)) n += fmt_num(line + n, sizeof line - n, v->custom[i]);
        else line[n++] = ',';
    }
    /* fuel and economy (econ.h): empty when the car gives nothing to work it out from */
    if (v->fuel_lph_calc >= 0) n += snprintf(line + n, sizeof line - n, "%.3f,%s,%.4f,%.4f,", v->fuel_lph_calc,
                                             fuel_src_name((fuel_src_t)v->fuel_src), v->trip_km, v->trip_fuel_l);
    else n += snprintf(line + n, sizeof line - n, ",,%.4f,%.4f,", v->trip_km, v->trip_fuel_l);
    n += snprintf(line + n, sizeof line - n, "%lu", (unsigned long)s_window_seq);
    line[n++] = '\n';
    line[n] = 0;

    if (fputs(line, s_csv) >= 0) { s_bytes += n; s_unsaved = true; s_last_write_ms = esp_timer_get_time() / 1000; }
    else { static int said; if (!said++) sys_event(EV_ERROR, "SD write failed: card full or removed?"); }
    bool saved = false;
    if (s_rows % CONFIG_SDLOG_FSYNC_EVERY_ROWS == 0 ||
        esp_timer_get_time() / 1000 - s_last_sync_ms >= (int64_t)CONFIG_SDLOG_SYNC_PERIOD_S * 1000) {
        sync_all_locked();
        saved = true;
    }
    xSemaphoreGive(s_lock);
    if (saved) sys_event(EV_INFO, "Safe to turn off");     /* same text repeats: the newest line just gets a fresh time */

    STATUS_LOCK();
    g_status.rows_written = s_rows;
    g_status.bytes_written = s_bytes;
    STATUS_UNLOCK();
}

void sdlog_write_raw(uint8_t pid, const char *hex)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open) { xSemaphoreGive(s_lock); return; }
    if (!s_raw) {
        snprintf(s_raw_path, sizeof s_raw_path, SDLOG_SESSIONS_DIR "/s%06lu.raw", (unsigned long)s_session);
        s_raw = fopen(s_raw_path, "a");
        if (s_raw) { fputs("uptime_ms,pid,hex\n", s_raw); s_raw_bytes = 18; }
    }
    if (s_raw) {
        int n = fprintf(s_raw, "%lld,01%02X,%s\n", (long long)(esp_timer_get_time() / 1000), pid, hex);
        if (n > 0) { s_raw_bytes += n; s_unsaved = true; }
    }
    xSemaphoreGive(s_lock);
}

void sdlog_set_window_seq(uint32_t seq) { s_window_seq = seq; }

void sdlog_write_pattern_window(uint32_t window_seq, int64_t start_ms, int64_t end_ms, uint16_t samples, int pattern_id,
                                float dist, float novelty, bool spawned, const char *sax_rpm, const char *sax_speed,
                                const float *features, int nfeatures)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open) { xSemaphoreGive(s_lock); return; }
    if (!s_pat) {
        snprintf(s_pat_path, sizeof s_pat_path, SDLOG_SESSIONS_DIR "/s%06lu.pat", (unsigned long)s_session);
        s_pat = fopen(s_pat_path, "a");
        if (s_pat) {
            int h = fprintf(s_pat, "window_seq,start_ms,end_ms,samples,pattern_id,dist,novelty,spawned,sax_rpm,sax_speed");
            for (int i = 0; i < nfeatures; i++) h += fprintf(s_pat, ",f%d", i);
            h += fprintf(s_pat, "\n");
            s_pat_bytes = h > 0 ? h : 0;
        }
    }
    if (s_pat) {
        int n = fprintf(s_pat, "%lu,%lld,%lld,%u,%d,%.3f,%.3f,%d,%s,%s", (unsigned long)window_seq, (long long)start_ms,
                        (long long)end_ms, samples, pattern_id, dist, novelty, spawned ? 1 : 0, sax_rpm, sax_speed);
        for (int i = 0; i < nfeatures; i++) n += fprintf(s_pat, ",%.4g", features[i]);
        n += fprintf(s_pat, "\n");
        if (n > 0) { s_pat_bytes += n; s_unsaved = true; }
    }
    xSemaphoreGive(s_lock);
}

void sdlog_write_ml_line(const char *header, const char *line)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open) { xSemaphoreGive(s_lock); return; }
    if (!s_ml) {
        snprintf(s_ml_path, sizeof s_ml_path, SDLOG_SESSIONS_DIR "/s%06lu.ml.csv", (unsigned long)s_session);
        s_ml = fopen(s_ml_path, "a");
        if (s_ml) { int h = fputs(header, s_ml); s_ml_bytes = h >= 0 ? strlen(header) : 0; }
    }
    if (s_ml && fputs(line, s_ml) >= 0) {
        s_ml_bytes += strlen(line);
        s_unsaved = true;
        /* flushed with the CSV in sync_all_locked(): one fsync per save, not one per model line */
    }
    xSemaphoreGive(s_lock);
}

bool sdlog_load_text(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return r > 0;
}

bool sdlog_save_text_atomic(const char *path, const char *text)
{
    char tmp[120];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs(text, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    remove(path);
    return rename(tmp, path) == 0;
}

void sdlog_prepare_sleep(void)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    meta_flush_locked();
    if (s_csv) { fflush(s_csv); fsync(fileno(s_csv)); fclose(s_csv); s_csv = NULL; }
    if (s_raw) { fflush(s_raw); fsync(fileno(s_raw)); fclose(s_raw); s_raw = NULL; }
    if (s_pat) { fflush(s_pat); fsync(fileno(s_pat)); fclose(s_pat); s_pat = NULL; }
    if (s_ml) { fflush(s_ml); fsync(fileno(s_ml)); fclose(s_ml); s_ml = NULL; }
    esp_vfs_fat_sdcard_unmount(CONFIG_SDLOG_MOUNT_POINT, s_card);
    s_ok = false;
    xSemaphoreGive(s_lock);
}

static void meta_set_str(cJSON *obj, const char *key, const char *val)
{
    if (!val) return;
    if (cJSON_HasObjectItem(obj, key)) cJSON_ReplaceItemInObject(obj, key, cJSON_CreateString(val));
    else cJSON_AddStringToObject(obj, key, val);
}

void sdlog_meta_set_adapter(const char *name, const char *addr, const char *ati, const char *batch)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *a = cJSON_GetObjectItem(s_meta, "adapter");
    meta_set_str(a, "name", name);
    meta_set_str(a, "addr", addr);
    meta_set_str(a, "firmware_id", ati);
    meta_set_str(a, "batch_mode", batch);
    meta_flush_locked();
    xSemaphoreGive(s_lock);
}

void sdlog_meta_set_vehicle(const char *vin, const char *protocol)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *veh = cJSON_GetObjectItem(s_meta, "vehicle");
    meta_set_str(veh, "vin", vin);
    meta_set_str(veh, "protocol", protocol);
    meta_flush_locked();
    xSemaphoreGive(s_lock);
}

void sdlog_meta_set_supported(const char *pid_cmd, const char *bitmap_hex)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *sp = cJSON_GetObjectItem(cJSON_GetObjectItem(s_meta, "vehicle"), "supported_pids");
    meta_set_str(sp, pid_cmd, bitmap_hex);
    meta_flush_locked();
    xSemaphoreGive(s_lock);
}

void sdlog_meta_set_time_sync(int64_t epoch_ms, int64_t uptime_ms)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_clock_source = "sntp";
    cJSON *ts = cJSON_GetObjectItem(s_meta, "time_sync");
    cJSON_ReplaceItemInObject(ts, "epoch_ms", cJSON_CreateNumber((double)epoch_ms));
    cJSON_ReplaceItemInObject(ts, "uptime_ms", cJSON_CreateNumber((double)uptime_ms));
    cJSON_ReplaceItemInObject(s_meta, "started_epoch_ms", cJSON_CreateNumber((double)(epoch_ms - uptime_ms)));
    meta_flush_locked();
    xSemaphoreGive(s_lock);
}

void sdlog_meta_add_dtcs(int64_t uptime_ms, const char *dtcs)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *hist = cJSON_GetObjectItem(s_meta, "dtc_history");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "uptime_ms", (double)uptime_ms);
    cJSON_AddStringToObject(e, "dtcs", dtcs ? dtcs : "");
    cJSON_AddItemToArray(hist, e);
    meta_flush_locked();
    xSemaphoreGive(s_lock);
}

void sdlog_meta_set_custom(size_t idx, const char *cmd, const char *name, const char *unit, const char *formula, const char *header)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *arr = cJSON_GetObjectItem(s_meta, "custom_pids");
    if (!arr) arr = cJSON_AddArrayToObject(s_meta, "custom_pids");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "column", (double)(idx + 1));
    cJSON_AddStringToObject(e, "cmd", cmd);
    cJSON_AddStringToObject(e, "name", name);
    cJSON_AddStringToObject(e, "unit", unit);
    cJSON_AddStringToObject(e, "formula", formula);
    cJSON_AddStringToObject(e, "header", header);
    cJSON_AddItemToArray(arr, e);
    meta_flush_locked();
    xSemaphoreGive(s_lock);
}

void sdlog_meta_set_faults(const char *insight, const char *ranked_codes, uint8_t max_severity)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *fa = cJSON_GetObjectItem(s_meta, "fault_analysis");
    if (!fa) fa = cJSON_AddObjectToObject(s_meta, "fault_analysis");
    meta_set_str(fa, "insight", insight);
    meta_set_str(fa, "ranked", ranked_codes);
    if (cJSON_HasObjectItem(fa, "max_severity")) cJSON_ReplaceItemInObject(fa, "max_severity", cJSON_CreateNumber(max_severity));
    else cJSON_AddNumberToObject(fa, "max_severity", max_severity);
    if (cJSON_HasObjectItem(fa, "uptime_ms")) cJSON_ReplaceItemInObject(fa, "uptime_ms", cJSON_CreateNumber((double)(esp_timer_get_time() / 1000)));
    else cJSON_AddNumberToObject(fa, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    meta_flush_locked();
    xSemaphoreGive(s_lock);
}
