#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#if !BOARD_HAS_SD
#include "diskio_wl.h"
#endif
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

#if BOARD_HAS_SD
static sdmmc_card_t *s_card;
#else
#include "wear_levelling.h"
static wl_handle_t s_wl = WL_INVALID_HANDLE;   /* no card slot (ESP-Mosaico): FAT "storage" partition in the NOR flash */
#endif
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
    "fuel_lph_calc,fuel_src,trip_km,trip_fuel_l,window_seq,fuel_lph_raw,fuel_cal,trip_fuel_raw_l,"
    "torque_nm,torque_src,tank_left_l,tank_range_km\n";
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
static const char *s_clock_source = "unset";   /* unset, rtc (kept through a reset), resumed (from the card), https (the time server) */

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

#if !BOARD_HAS_SD
/* ---------- boot-time storage audit (2026-11-11): the old TinyUSB MSC door had formatted "storage" with one FAT and an
   MBR; one torn FAT-sector update (erase done, rewrite lost) left the FAT all 0xFFF = every cluster "end of chain",
   so the volume showed 0 KB free while its files held 94 KB. This compares the space the files need (whole clusters)
   with the space the FAT says is used, and names a single-FAT volume, so a repeat is seen at the next boot. ---------- */
static uint64_t tree_bytes(const char *dir, uint32_t clu, int *files, int depth)
{
    uint64_t sum = clu;                                   /* the directory's own cluster (the root's is outside the data area: close enough) */
    DIR *d = depth < 6 ? opendir(dir) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        char p[160];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) sum += tree_bytes(p, clu, files, depth + 1);
        else { sum += ((uint64_t)st.st_size + clu - 1) / clu * clu; (*files)++; }
    }
    if (d) closedir(d);
    return sum;
}

static void storage_audit(void)
{
    char drv[4] = {(char)('0' + ff_diskio_get_pdrv_wl(s_wl)), ':', 0};
    FATFS *fs = NULL;
    DWORD free_cl = 0;
    if (f_getfree(drv, &free_cl, &fs) != FR_OK || !fs) { ESP_LOGW(TAG, "storage audit: f_getfree failed"); return; }
    uint32_t clu = (uint32_t)fs->csize * FF_MAX_SS;
    uint64_t total = (uint64_t)(fs->n_fatent - 2) * clu, freeb = (uint64_t)free_cl * clu, used = total - freeb;
    int files = 0;
    uint64_t need = tree_bytes(CONFIG_SDLOG_MOUNT_POINT, clu, &files, 0);
    bool bad = used > need + need / 10 + 2 * clu;          /* more than 10 % (and two clusters) used beyond what the files need */
    ESP_LOGI(TAG, "storage audit: FAT%d, %d FAT cop%s, %lu B clusters: %llu KB used by the FAT, %llu KB needed by %d files, %llu of %llu KB free%s",
             fs->fs_type == FS_FAT12 ? 12 : fs->fs_type == FS_FAT16 ? 16 : 32, fs->n_fats, fs->n_fats == 1 ? "y" : "ies",
             (unsigned long)clu, (unsigned long long)(used / 1024), (unsigned long long)(need / 1024), files,
             (unsigned long long)(freeb / 1024), (unsigned long long)(total / 1024), bad ? "  <- LOST CLUSTERS" : "");
    if (bad) sys_event(EV_ERROR, "Log storage: %llu KB used but files need %llu KB (lost clusters): format storage in Settings",
                       (unsigned long long)(used / 1024), (unsigned long long)(need / 1024));
    if (fs->n_fats < 2) sys_event(EV_WARN, "Log storage has one FAT copy (old USB-disk format): format storage in Settings");
#ifdef SDLOG_SELFTEST
    /* one-off proof after a format: write 64 KB, close, delete; free space must come back exactly */
    static uint8_t blk[4096];
    memset(blk, 0xA5, sizeof blk);
    FILE *f = fopen(SDLOG_DIR "/selftest.bin", "wb");
    int wr = 0;
    for (int i = 0; f && i < 16; i++) wr += fwrite(blk, 1, sizeof blk, f) == sizeof blk;
    if (f) { fflush(f); fsync(fileno(f)); fclose(f); }
    DWORD mid = 0, end = 0;
    f_getfree(drv, &mid, &fs);
    unlink(SDLOG_DIR "/selftest.bin");
    f_getfree(drv, &end, &fs);
    ESP_LOGW(TAG, "storage selftest: wrote %d of 16 x 4 KB, free %lu -> %lu -> %lu clusters: %s", wr, (unsigned long)free_cl,
             (unsigned long)mid, (unsigned long)end, wr == 16 && end == free_cl && mid + 16 <= free_cl ? "PASS" : "FAIL");
#endif
}
#endif

/* ---------- public ---------- */
esp_err_t sdlog_init(void)
{
    s_lock = xSemaphoreCreateMutex();

#if BOARD_HAS_SD
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
#else
    /* the wear-levelled FAT on "storage" is formatted on first use (nothing to lose: it is the board's own log space) */
    const esp_vfs_fat_mount_config_t mount_config = { .format_if_mount_failed = true, .max_files = 8,
                                                      .allocation_unit_size = CONFIG_WL_SECTOR_SIZE };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(CONFIG_SDLOG_MOUNT_POINT, "storage", &mount_config, &s_wl);
#endif
    if (err != ESP_OK) {
        sys_event(EV_ERROR, "SD card mount failed: %s", esp_err_to_name(err));
        STATUS_LOCK();
        g_status.sd_mounted = false;
        snprintf(g_status.sd_error, sizeof g_status.sd_error, "%s", esp_err_to_name(err));
        STATUS_UNLOCK();
        return err;
    }
#if BOARD_HAS_SD
    sdmmc_card_print_info(stdout, s_card);
#else
    {
        uint64_t total = 0, free_b = 0;
        esp_vfs_fat_info(CONFIG_SDLOG_MOUNT_POINT, &total, &free_b);
        ESP_LOGI(TAG, "flash FAT \"storage\" at %s: %llu of %llu KB free", CONFIG_SDLOG_MOUNT_POINT,
                 (unsigned long long)(free_b / 1024), (unsigned long long)(total / 1024));
    }
#endif

    mkdir(SDLOG_DIR, 0775);
    mkdir(SDLOG_SESSIONS_DIR, 0775);
#if !BOARD_HAS_SD
    storage_audit();
#endif
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
    n += snprintf(line + n, sizeof line - n, "%lu,", (unsigned long)s_window_seq);
    /* appended in 2.5: the fuel rate before the fill-up calibration, the factor, and the raw trip total */
    if (v->fuel_lph_raw >= 0) n += snprintf(line + n, sizeof line - n, "%.3f,", v->fuel_lph_raw);
    else line[n++] = ',';
    n += snprintf(line + n, sizeof line - n, "%.4f,%.4f,", v->fuel_cal, v->trip_fuel_raw_l);
    /* appended in 2.5: R.E.V physics torque (5 s mean) and the tank learner, empty when unknown */
    static const char *tq_src[] = {"", "pid", "rate", "maf", "k"};
    if (isfinite(v->torque_nm)) n += snprintf(line + n, sizeof line - n, "%.1f,%s,", v->torque_nm, tq_src[v->torque_src < 5 ? v->torque_src : 0]);
    else n += snprintf(line + n, sizeof line - n, ",,");
    if (v->tank_left_l >= 0) n += snprintf(line + n, sizeof line - n, "%.1f,", v->tank_left_l); else line[n++] = ',';
    if (v->tank_range_km >= 0) n += snprintf(line + n, sizeof line - n, "%.0f", v->tank_range_km);
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

bool session_name_valid(const char *n)
{
    if (!n || strlen(n) < 11 || n[0] != 's') return false;
    for (int i = 1; i <= 6; i++) if (n[i] < '0' || n[i] > '9') return false;
    const char *e = n + 7;
    return !strcmp(e, ".csv") || !strcmp(e, ".ml.csv") || !strcmp(e, ".pat") || !strcmp(e, ".raw") || !strcmp(e, ".json");
}

/* lock held; id = 0 means every session but the current one */
static int delete_locked(uint32_t id, int *sessions)
{
    DIR *d = opendir(SDLOG_SESSIONS_DIR);
    if (!d) return -1;
    int files = 0;
    if (sessions) *sessions = 0;
    struct dirent *e;
    EXT_RAM_BSS_ATTR static char names[512][24];      /* collected first: no remove() while readdir() walks the directory */
    int n = 0;
    while ((e = readdir(d)) != NULL && n < 512) {
        if (!session_name_valid(e->d_name)) continue;
        unsigned long sid = strtoul(e->d_name + 1, NULL, 10);
        if (sid == s_session) continue;                 /* never the session being written */
        if (id && sid != id) continue;
        snprintf(names[n++], sizeof names[0], "%.20s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char path[96];
        snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%s", names[i]);
        if (remove(path) != 0) continue;
        files++;
        bool first = true;                              /* count each session once */
        for (int k = 0; k < i && first; k++) if (!strncmp(names[k], names[i], 7)) first = false;
        if (sessions && first) (*sessions)++;
    }
    return files;
}

int sdlog_delete_session(uint32_t id)
{
    if (!s_ok || id == 0 || id == s_session) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int files = delete_locked(id, NULL);
    xSemaphoreGive(s_lock);
    if (files >= 0) sys_event(EV_INFO, "Drive log s%06lu deleted on the board (%d files)", (unsigned long)id, files);
    return files;
}

int sdlog_delete_all_sessions(int *sessions_out)
{
    if (!s_ok) return -1;
    int sessions = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int files = delete_locked(0, &sessions);
    xSemaphoreGive(s_lock);
    if (sessions_out) *sessions_out = sessions;
    if (files >= 0) sys_event(EV_INFO, "All drive logs deleted on the board: %d files (current session kept)", files);
    return files;
}

#if !BOARD_HAS_SD
esp_err_t sdlog_format_storage(uint32_t *free_kb, int *kept_files, char *why, size_t why_n)
{
    if (!s_ok) { snprintf(why, why_n, "storage not mounted"); return ESP_ERR_INVALID_STATE; }
    if (s_open) { snprintf(why, why_n, "a drive is being logged"); return ESP_ERR_INVALID_STATE; }
    typedef struct { char name[48]; uint8_t *data; size_t n; } keep_t;
    static keep_t keep[24];
    int nk = 0;
    size_t total = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    meta_flush_locked();
    if (s_csv) { fclose(s_csv); s_csv = NULL; }
    if (s_raw) { fclose(s_raw); s_raw = NULL; }
    if (s_pat) { fclose(s_pat); s_pat = NULL; }
    if (s_ml) { fclose(s_ml); s_ml = NULL; }
    DIR *d = opendir(SDLOG_DIR);
    for (struct dirent *e; d && (e = readdir(d)) != NULL && nk < 24;) {
        if (!strcmp(e->d_name, "drives.csv") || strlen(e->d_name) >= sizeof keep[0].name) continue;
        char path[96];
        struct stat st;
        snprintf(path, sizeof path, SDLOG_DIR "/%s", e->d_name);
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size <= 0 || total + st.st_size > 512 * 1024) continue;
        uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
        FILE *f = buf ? fopen(path, "rb") : NULL;
        size_t n = f ? fread(buf, 1, st.st_size, f) : 0;
        if (f) fclose(f);
        if (n != (size_t)st.st_size) { free(buf); continue; }
        snprintf(keep[nk].name, sizeof keep[nk].name, "%s", e->d_name);
        keep[nk].data = buf; keep[nk].n = n; nk++;
        total += n;
    }
    if (d) closedir(d);
    esp_err_t err = esp_vfs_fat_spiflash_format_rw_wl(CONFIG_SDLOG_MOUNT_POINT, "storage");
    int back = 0;
    if (err == ESP_OK) {
        mkdir(SDLOG_DIR, 0775);
        mkdir(SDLOG_SESSIONS_DIR, 0775);
        for (int i = 0; i < nk; i++) {
            char path[96];
            snprintf(path, sizeof path, SDLOG_DIR "/%s", keep[i].name);
            FILE *f = fopen(path, "wb");
            if (f && fwrite(keep[i].data, 1, keep[i].n, f) == keep[i].n) back++;
            if (f) fclose(f);
        }
    } else snprintf(why, why_n, "format failed: %s", esp_err_to_name(err));
    for (int i = 0; i < nk; i++) free(keep[i].data);
    uint64_t tot = 0, fr = 0;
    esp_vfs_fat_info(CONFIG_SDLOG_MOUNT_POINT, &tot, &fr);
    xSemaphoreGive(s_lock);
    if (free_kb) *free_kb = (uint32_t)(fr / 1024);
    if (kept_files) *kept_files = back;
    if (err == ESP_OK) sys_event(EV_INFO, "Log storage formatted on the board: %d of %d state files kept, %lu KB free", back, nk,
                                 (unsigned long)(fr / 1024));
    return err;
}
#endif

void sdlog_prepare_sleep(void)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    meta_flush_locked();
    if (s_csv) { fflush(s_csv); fsync(fileno(s_csv)); fclose(s_csv); s_csv = NULL; }
    if (s_raw) { fflush(s_raw); fsync(fileno(s_raw)); fclose(s_raw); s_raw = NULL; }
    if (s_pat) { fflush(s_pat); fsync(fileno(s_pat)); fclose(s_pat); s_pat = NULL; }
    if (s_ml) { fflush(s_ml); fsync(fileno(s_ml)); fclose(s_ml); s_ml = NULL; }
#if BOARD_HAS_SD
    esp_vfs_fat_sdcard_unmount(CONFIG_SDLOG_MOUNT_POINT, s_card);
#else
    esp_vfs_fat_spiflash_unmount_rw_wl(CONFIG_SDLOG_MOUNT_POINT, s_wl);
    s_wl = WL_INVALID_HANDLE;
#endif
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

void sdlog_meta_set_ml(const char *model_key, float fuel_cal)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_meta_string(s_meta, "ml_model", model_key);
    if (cJSON_HasObjectItem(s_meta, "fuel_cal")) cJSON_ReplaceItemInObject(s_meta, "fuel_cal", cJSON_CreateNumber(fuel_cal));
    else cJSON_AddNumberToObject(s_meta, "fuel_cal", fuel_cal);
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
    s_clock_source = "https";
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

void sdlog_meta_set_json(const char *key, const char *json)
{
    if (!s_meta) return;
    cJSON *v = cJSON_Parse(json);
    if (!v) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (cJSON_HasObjectItem(s_meta, key)) cJSON_ReplaceItemInObject(s_meta, key, v);
    else cJSON_AddItemToObject(s_meta, key, v);
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
