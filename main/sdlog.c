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
#include "esp_partition.h"
#include "fat_mirror.h"
#include "session_next.h"
#include "sched_mon.h"
#endif
#if CONFIG_OCM_NAND_LOGS
#include "nand_store.h"
#include "log_store.h"
void log_move_drives_csv(void);
void log_move_start(void);
#endif
#include "esp_system.h"
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
static volatile bool s_quiesced;  /* sdlog_quiesce_for_reset() ran: a reset follows, nothing more is written */
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

#if CONFIG_OCM_NAND_LOGS
/* With the NAND mounted clock.txt lives there and is written at most once a minute (log_store_clock_due): every NOR write
   stalls the flash cache, and this was the last one at every save. The NOR copy stays as the fallback for a boot without
   the NAND; at boot the newer of the two wins. */
#define NAND_CLOCK_PATH NAND_LOG_DIR "/clock.txt"
static int64_t s_clock_saved_ms = -1;          /* uptime of the last clock.txt write, -1 = none since boot */
static bool s_clock_force;                     /* the clock was just set (by hand or by Wi-Fi): write at the next save */

static void save_clock_locked(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (!harden_epoch_plausible(tv.tv_sec)) return;
    int64_t now = esp_timer_get_time() / 1000;
    if (!log_store_clock_due(now, s_clock_saved_ms, s_clock_force)) return;
    FILE *f = fopen(nand_store_mounted() ? NAND_CLOCK_PATH : CLOCK_PATH, "w");
    if (!f) return;
    fprintf(f, "%lld\n", (long long)tv.tv_sec);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    s_clock_saved_ms = now;
    s_clock_force = false;
}

static long long read_clock_file(const char *path)   /* 0 when missing or not a plausible time */
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long saved = 0;
    int n = fscanf(f, "%lld", &saved);
    fclose(f);
    return n == 1 && harden_epoch_plausible(saved) ? saved : 0;
}

static void restore_clock(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    if (harden_epoch_plausible(now.tv_sec)) { s_clock_source = "rtc"; return; }
    long long saved = log_store_clock_pick(nand_store_mounted() ? read_clock_file(NAND_CLOCK_PATH) : 0, read_clock_file(CLOCK_PATH));
    if (!saved) return;
    struct timeval tv = { .tv_sec = (time_t)saved, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_clock_source = "resumed";
    STATUS_LOCK(); g_status.clock_estimated = true; STATUS_UNLOCK();
    sys_event(EV_INFO, "Clock resumed from the card, Wi-Fi will correct it");
}
#else
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

#endif

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
#if !BOARD_HAS_SD
        /* a re-flash erases NVS but keeps the storage partition: never reuse the number of a drive that is still there */
        uint32_t hi = session_highest_in_dir(SDLOG_SESSIONS_DIR), next = session_next_id(id, hi);
#if CONFIG_OCM_NAND_LOGS
        uint32_t hi_nand = nand_store_mounted() ? session_highest_in_dir(NAND_SESSIONS_DIR) : 0;   /* both stores */
        next = log_store_next_session(id, hi_nand, hi);
        if (hi_nand > hi) hi = hi_nand;
#endif
        if (next != id + 1) ESP_LOGW(TAG, "session counter %lu is behind the storage (highest s%06lu): next is %lu",
                                     (unsigned long)id, (unsigned long)hi, (unsigned long)next);
        id = next;
#else
        id++;
#endif
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
   needs far more stack than the FreeRTOS timer task has. With an SD card (the S3) the stack is in PSRAM; on the ESP-Mosaico
   it is internal (its saves write the NOR flash, which turns the flash cache off: see sdlog_init). */
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
    SDLOG_DRIVE_PATH(s_csv_path, "/s%06lu.csv", (unsigned long)s_session);
    SDLOG_DRIVE_PATH(s_json_path, "/s%06lu.json", (unsigned long)s_session);

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

/* ---------- flash FAT checks before FatFs mounts (see fat_mirror.h for why) ---------- */
static int  s_fat_repairs;        /* FAT sectors rewritten from the other copy at this boot */
static bool s_unreadable;         /* the volume holds data FatFs cannot use: left unmounted, nothing overwritten */
static uint32_t s_reclaimed_kb;    /* lost clusters freed at this boot */
static int      s_scrubbed;        /* torn directory entries marked deleted at this boot */

static bool wl_sector_read(uint32_t lba, uint8_t *buf, void *ctx)
{
    wl_handle_t h = *(wl_handle_t *)ctx;
    return wl_read(h, (size_t)lba * wl_sector_size(h), buf, wl_sector_size(h)) == ESP_OK;
}

static bool wl_sector_write(wl_handle_t h, uint32_t lba, const uint8_t *buf)
{
    size_t ss = wl_sector_size(h);
    return wl_erase_range(h, (size_t)lba * ss, ss) == ESP_OK && wl_write(h, (size_t)lba * ss, buf, ss) == ESP_OK;
}

typedef enum { PRE_OK, PRE_NO_FAT, PRE_BOTH_BAD, PRE_ERROR } preflight_t;

static bool wl_sector_write_cb(uint32_t lba, const uint8_t *buf, void *ctx) { return wl_sector_write(*(wl_handle_t *)ctx, lba, buf); }

/* Frees clusters no directory entry reaches (fat_reclaim: chains of files a reset caught before their directory entry was
   written). FAT 1 is written before FAT 2, as FatFs does, so a reset in between is undone by the copy check next boot. */
static void storage_reclaim(wl_handle_t h, const fat_geom_t *g, uint8_t *sec)
{
    size_t ss = g->sector_size, n = (size_t)g->fat_sectors * ss;
    if (n > 64 * 1024) return;
    uint8_t *fat = heap_caps_malloc(n, MALLOC_CAP_SPIRAM), *orig = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    uint8_t *mark = heap_caps_calloc(1, FAT_RECLAIM_MARK_BYTES(g), MALLOC_CAP_SPIRAM);
    bool ok = fat && orig && mark;
    for (uint32_t i = 0; ok && i < g->fat_sectors; i++) ok = wl_sector_read(g->fat_start + i, fat + i * ss, &h);
    int scrubbed = 0;
    int freed = ok ? (memcpy(orig, fat, n), fat_reclaim(g, fat, wl_sector_read, wl_sector_write_cb, &h, sec, mark, &scrubbed)) : -1;
    if (scrubbed) {
        s_scrubbed = scrubbed;
        ESP_LOGE(TAG, "storage: a directory sector was torn by a reset: %d broken entr%s marked deleted (those files are lost)",
                 scrubbed, scrubbed == 1 ? "y" : "ies");
    }
    if (freed < 0) ESP_LOGW(TAG, "storage: directory tree not clean: lost clusters left as they are");
    for (int copy = 0; freed > 0 && copy < g->n_fats; copy++)
        for (uint32_t i = 0; i < g->fat_sectors; i++)
            if (memcmp(fat + i * ss, orig + i * ss, ss) &&
                !wl_sector_write(h, g->fat_start + copy * g->fat_sectors + i, fat + i * ss)) { freed = -1; break; }
    if (freed > 0) {
        s_reclaimed_kb = (uint32_t)((uint64_t)freed * g->sec_per_clus * ss / 1024);
        ESP_LOGW(TAG, "storage: %d lost cluster%s (%lu KB) from interrupted writes freed", freed, freed == 1 ? "" : "s",
                 (unsigned long)s_reclaimed_kb);
    }
    free(fat);
    free(orig);
    free(mark);
}

/* Compares the two FAT copies sector by sector through the wear-levelling layer and rewrites a torn copy from the good
   one, before FatFs reads either (FatFs never reads FAT 2, ff.c sync_window). Only the damaged copy is written, so a
   reset during the repair leaves the good copy for the next boot. */
static preflight_t storage_preflight(const esp_partition_t *part)
{
    wl_handle_t h = WL_INVALID_HANDLE;
    if (wl_mount(part, &h) != ESP_OK) return PRE_ERROR;
    size_t ss = wl_sector_size(h);
    uint8_t *a = heap_caps_malloc(ss, MALLOC_CAP_SPIRAM), *b = heap_caps_malloc(ss, MALLOC_CAP_SPIRAM);
    uint8_t *c = heap_caps_malloc(ss, MALLOC_CAP_SPIRAM);
    preflight_t r = PRE_OK;
    fat_geom_t g;
    if (!a || !b || !c || !wl_sector_read(0, a, &h)) r = PRE_ERROR;
    else if (!fat_geom_parse(a, ss, wl_sector_read, &h, c, &g)) r = PRE_NO_FAT;
    else if (g.sector_size != ss) ESP_LOGW(TAG, "storage: FAT sector size %lu differs from the flash layer's %u: FAT copies not checked",
                                           (unsigned long)g.sector_size, (unsigned)ss);
    else {
        for (uint32_t i = 0; i < g.fat_sectors && r == PRE_OK; i++) {
            uint32_t s1 = g.fat_start + i, s2 = s1 + g.fat_sectors;
            if (!wl_sector_read(s1, a, &h) || (g.n_fats == 2 && !wl_sector_read(s2, b, &h))) { r = PRE_ERROR; break; }
            fat_verdict_t v = fat_mirror_compare(a, g.n_fats == 2 ? b : a, ss, i == 0, g.media);
            if (v == FAT_BOTH_BAD) r = PRE_BOTH_BAD;
            else if (v == FAT_COPY_2_TO_1 || v == FAT_COPY_1_TO_2) {
                bool to1 = v == FAT_COPY_2_TO_1;
                bool ok = wl_sector_write(h, to1 ? s1 : s2, to1 ? b : a);
                s_fat_repairs += ok;
                ESP_LOGE(TAG, "storage: FAT %d sector %lu was torn by a reset: rewritten from FAT %d: %s", to1 ? 1 : 2,
                         (unsigned long)i, to1 ? 2 : 1, ok ? "ok" : "FAILED");
                if (!ok) r = PRE_ERROR;
            }
        }
        if (r == PRE_OK) storage_reclaim(h, &g, a);
    }
    free(a);
    free(b);
    free(c);
    wl_unmount(h);
    return r;
}

/* Nothing but erased flash in the wear-levelling data area: a first boot, or a partition blanked on purpose. The WL
   metadata at the end (two state copies and the config sector) is skipped; its size follows WL_Flash.cpp config(),
   lines 75-85 (16 B position records, the default wl_pos_update_record_size). */
static bool storage_blank(const esp_partition_t *part)
{
    const uint32_t sec = 4096;
    uint32_t state = 64 + part->size / sec * 16;
    state = (state + sec - 1) / sec * sec;
    uint32_t end = part->size - 2 * state - sec;
    uint32_t *buf = heap_caps_malloc(sec, MALLOC_CAP_SPIRAM);
    bool blank = buf != NULL;
    for (uint32_t off = 0; blank && off < end; off += sec) {
        if (esp_partition_read(part, off, buf, sec) != ESP_OK) { blank = false; break; }
        for (uint32_t i = 0; i < sec / 4 && blank; i++) blank = buf[i] == 0xFFFFFFFFu;
    }
    free(buf);
    return blank;
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
    if (bad) sys_event(EV_ERROR, "Flash FAT: %llu KB used, files %llu KB: Format it",
                       (unsigned long long)(used / 1024), (unsigned long long)(need / 1024));
    if (fs->n_fats < 2) sys_event(EV_WARN, "Flash FAT has one FAT copy (old format): Format it");
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
#if !BOARD_HAS_SD
/* sdlog_init's own writes (the FAT copy repair, the orphan reclaim and torn-entry scrub, mkdir, the empty-session prune,
   the new session's files) run before s_ok is set, while the console's download door is already up (app_main starts
   TinyUSB first). A download entry in that window used to reset in the middle of one of them: measured 2026-11-11, a
   directory sector torn by a door entry a second after boot. The quiesce now waits for sdlog_init to finish. */
static volatile bool s_initing;
static esp_err_t sdlog_init_inner(void);
esp_err_t sdlog_init(void)
{
    s_initing = true;
    esp_err_t e = sdlog_init_inner();
    s_initing = false;
    return e;
}
static esp_err_t sdlog_init_inner(void)
#else
esp_err_t sdlog_init(void)
#endif
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
    /* The wear-levelled FAT on "storage". Torn FAT copies are repaired before FatFs mounts. The partition is formatted
       only when it is blank (first use): a volume that holds data but does not mount stays untouched, so the logs can
       still be read out raw, and only the owner's "Format storage" in Settings erases it. */
    esp_vfs_fat_mount_config_t mount_config = { .format_if_mount_failed = false, .max_files = 8,
                                                .allocation_unit_size = CONFIG_WL_SECTOR_SIZE };
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    preflight_t pre = part ? storage_preflight(part) : PRE_ERROR;
    esp_err_t err = ESP_FAIL;
    if (pre == PRE_BOTH_BAD) {
        s_unreadable = true;
        ESP_LOGE(TAG, "storage: BOTH FAT copies are damaged: not mounted, nothing written; the files are still in the flash "
                      "(read them out raw), then use Format storage in Settings");
    } else if (part) {
        err = esp_vfs_fat_spiflash_mount_rw_wl(CONFIG_SDLOG_MOUNT_POINT, "storage", &mount_config, &s_wl);
        if (err != ESP_OK && s_wl != WL_INVALID_HANDLE) { wl_unmount(s_wl); s_wl = WL_INVALID_HANDLE; }
        if (err == ESP_FAIL && storage_blank(part)) {
            ESP_LOGW(TAG, "storage: blank partition, formatting it (first use)");
            mount_config.format_if_mount_failed = true;
            err = esp_vfs_fat_spiflash_mount_rw_wl(CONFIG_SDLOG_MOUNT_POINT, "storage", &mount_config, &s_wl);
        } else if (err == ESP_FAIL) {
            s_unreadable = true;
            ESP_LOGE(TAG, "storage: holds data but has no FAT FatFs can mount: not formatted, nothing written; read it out "
                          "raw, then use Format storage in Settings");
        }
    }
    if (s_unreadable) sys_event(EV_ERROR, "Flash FAT unreadable, left untouched. Format "
                                          "erases it");
    if (err == ESP_OK) esp_register_shutdown_handler(sdlog_quiesce_for_reset);   /* every esp_restart() */
    if (err == ESP_OK && s_scrubbed)
        sys_event(EV_ERROR, "Flash FAT: reset tore a folder, %d files "
                            "lost", s_scrubbed);
    if (err == ESP_OK && s_fat_repairs)
        sys_event(EV_WARN, "Flash FAT: %d FAT sector%.1s torn by a reset, repaired", s_fat_repairs,
                  s_fat_repairs == 1 ? "" : "s");
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
#if CONFIG_OCM_NAND_LOGS
    if (nand_store_init() == ESP_OK) {               /* drive store; without it the drive logs stay on the flash FAT */
        mkdir(NAND_LOG_DIR, 0775);
        mkdir(NAND_SESSIONS_DIR, 0775);
        log_move_drives_csv();                       /* small: done here, before any drive can append to it */
    }
#endif
    restore_clock();                  /* before the session opens, so its start time is not 1970 */

    /* Remove sessions from earlier boots that never logged a row (header-only CSV, no raw file). */
    {
        DIR *d = opendir(SDLOG_DRIVE_DIR);
        struct dirent *e;
        int pruned = 0;
        while (d && (e = readdir(d)) != NULL) {
            unsigned long id;
            if (strlen(e->d_name) != 11 || e->d_name[0] != 's' || strcmp(e->d_name + 7, ".csv") != 0 ||
                sscanf(e->d_name + 1, "%06lu", &id) != 1) continue;
            char path[96];
            struct stat st;
            SDLOG_DRIVE_PATH(path, "/%.16s", e->d_name);
            if (stat(path, &st) != 0 || st.st_size > (off_t)strlen(CSV_HEADER)) continue;
            SDLOG_DRIVE_PATH(path, "/s%06lu.raw", id);
            if (stat(path, &st) == 0) continue;
            remove(path);
            SDLOG_DRIVE_PATH(path, "/s%06lu.csv", id);  remove(path);
            SDLOG_DRIVE_PATH(path, "/s%06lu.json", id); remove(path);
            SDLOG_DRIVE_PATH(path, "/s%06lu.pat", id);  remove(path);
            SDLOG_DRIVE_PATH(path, "/s%06lu.ml.csv", id); remove(path);
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
#if BOARD_HAS_SD
    xTaskCreateWithCaps(sync_task, "sdsync", 6144, NULL, 2, NULL, MALLOC_CAP_SPIRAM);
#else
    /* The ESP-Mosaico keeps its logs in the NOR flash: every log write disables the flash cache, and a task with a PSRAM
       stack then trips esp_task_stack_is_sane_cache_disabled(). With an SD card the stack stays in PSRAM. */
    xTaskCreate(sync_task, "sdsync", 6144, NULL, 2, NULL);
#endif
#if CONFIG_OCM_NAND_LOGS
    log_move_start();                                /* drive logs still on the flash FAT -> the NAND, in the background */
#endif
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
        SDLOG_DRIVE_PATH(s_raw_path, "/s%06lu.raw", (unsigned long)s_session);
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
        SDLOG_DRIVE_PATH(s_pat_path, "/s%06lu.pat", (unsigned long)s_session);
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
        SDLOG_DRIVE_PATH(s_ml_path, "/s%06lu.ml.csv", (unsigned long)s_session);
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
    return !strcmp(e, ".csv") || !strcmp(e, ".ml.csv") || !strcmp(e, ".pat") || !strcmp(e, ".raw") || !strcmp(e, ".json") ||
           !strcmp(e, ".sum.json");                     /* the ESP-Mosaico's drive summary (mosaico_drive.c) */
}

/* lock held; id = 0 means every session but the current one */
#if CONFIG_OCM_NAND_LOGS
static int delete_in_dir_locked(const char *dir, uint32_t id, int *sessions);
static int delete_locked(uint32_t id, int *sessions)
{
    const char *dirs[2];
    int nd = sdlog_drive_dirs(dirs), files = 0, any = 0;
    if (sessions) *sessions = 0;
    for (int k = 0; k < nd; k++) {
        int s = 0, f = delete_in_dir_locked(dirs[k], id, &s);
        if (f < 0) continue;
        any = 1;
        files += f;
        if (sessions) *sessions += s;
    }
    return any ? files : -1;
}
static int delete_in_dir_locked(const char *dir, uint32_t id, int *sessions)
{
    DIR *d = opendir(dir);
#else
static int delete_locked(uint32_t id, int *sessions)
{
    DIR *d = opendir(SDLOG_SESSIONS_DIR);
#endif
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
#if CONFIG_OCM_NAND_LOGS
        snprintf(path, sizeof path, "%.48s/%.23s", dir, names[i]);
#else
        snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%s", names[i]);
#endif
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
    if (files >= 0) sys_event(EV_INFO, "All drive logs deleted: %d files (current kept)", files);
    return files;
}

#if !BOARD_HAS_SD
#if CONFIG_OCM_NAND_LOGS
/* The NAND is the drive store: format it (erase + FAT, ~15 s), and remove the drive logs still on the flash FAT (drives
   not moved yet, or logged while the NAND was down). The flash FAT and its state files are not touched. */
static esp_err_t format_drive_store(uint32_t *free_kb, int *kept_files, char *why, size_t why_n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    meta_flush_locked();
    if (s_csv) { fclose(s_csv); s_csv = NULL; }
    if (s_raw) { fclose(s_raw); s_raw = NULL; }
    if (s_pat) { fclose(s_pat); s_pat = NULL; }
    if (s_ml) { fclose(s_ml); s_ml = NULL; }
    s_open = false;                                   /* the next drive opens a new session, on the formatted NAND */
    esp_err_t err = nand_store_format(why, why_n);
    if (err == ESP_OK) {
        mkdir(NAND_LOG_DIR, 0775);
        mkdir(NAND_SESSIONS_DIR, 0775);
        delete_in_dir_locked(SDLOG_SESSIONS_DIR, 0, NULL);
        remove(SDLOG_DIR "/drives.csv");
        s_clock_saved_ms = -1;                        /* clock.txt went with the NAND: write it again at the next save */
    }
    uint64_t tot = 0, fr = 0;
    nand_store_info(&tot, &fr);
    int state = 0;                                    /* state files on the flash FAT, untouched */
    DIR *d = opendir(SDLOG_DIR);
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) state += e->d_type == DT_REG;
    if (d) closedir(d);
    xSemaphoreGive(s_lock);
    if (free_kb) *free_kb = (uint32_t)(fr / 1024);
    if (kept_files) *kept_files = state;
    if (err == ESP_OK) sys_event(EV_INFO, "Drive store formatted: %lu KB, %d state files "
                                 "kept", (unsigned long)(fr / 1024), state);
    return err;
}
#endif
esp_err_t sdlog_format_storage(uint32_t *free_kb, int *kept_files, char *why, size_t why_n)
{
    if (!s_ok && s_unreadable) {
        /* the owner chose to erase a damaged volume (sdlog_init left it unmounted): format, then restart so the logger
           starts on it like on any boot */
        esp_err_t err = esp_vfs_fat_spiflash_format_rw_wl(CONFIG_SDLOG_MOUNT_POINT, "storage");
        ESP_LOGW(TAG, "damaged storage formatted on request: %s; restarting", esp_err_to_name(err));
        if (err != ESP_OK) { snprintf(why, why_n, "format failed: %s", esp_err_to_name(err)); return err; }
        vTaskDelay(pdMS_TO_TICKS(300));
        sched_mark_intentional_reset();
        esp_restart();
    }
    if (!s_ok) { snprintf(why, why_n, "storage not mounted"); return ESP_ERR_INVALID_STATE; }
#if CONFIG_OCM_NAND_LOGS
    /* the session opened at boot stays open (s_open) until a drive with rows ends; one without rows holds no data */
    if (nand_store_state() != NAND_ABSENT) {
        if (!log_store_format_allowed(s_open, s_rows)) { snprintf(why, why_n, "a drive is being logged"); return ESP_ERR_INVALID_STATE; }
        return format_drive_store(free_kb, kept_files, why, why_n);
    }
    /* no NAND: the flash FAT path. Like the NAND path, the session opened at boot with no rows does not block it (it was
       refused here right after every boot, until a drive ended) */
    if (!log_store_format_allowed(s_open, s_rows)) { snprintf(why, why_n, "a drive is being logged"); return ESP_ERR_INVALID_STATE; }
#else
    if (s_open && s_rows) { snprintf(why, why_n, "a drive is being logged"); return ESP_ERR_INVALID_STATE; }
#endif
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
    s_open = false;                                   /* its files go with the format: the next drive opens a new session */
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
    if (err == ESP_OK) sys_event(EV_INFO, "Flash FAT formatted: %d of %d state files kept", back,
                                 nk);
    return err;
}
#endif

#if CONFIG_OCM_NAND_LOGS
/* the flash FAT part (below, unchanged), then the NAND: its lock comes after the logger's own flush, because that flush
   (fsync of the drive files) runs on the NAND volume and FatFs's volume lock is not recursive */
static void quiesce_flash_fat(void);
void sdlog_quiesce_for_reset(void)
{
    quiesce_flash_fat();
    nand_store_quiesce();
}
static void quiesce_flash_fat(void)
#else
void sdlog_quiesce_for_reset(void)
#endif
{
    /* The flash FAT writes a sector as erase-then-program (diskio_wl.c ff_wl_write), so a reset between the two leaves
       an erased sector. Here: flush the logger's files under its lock, mark it stopped (its writers return at s_ok), and
       take FatFs's own volume lock (ff_mutex_take, the lock every f_* call takes in lock_volume) without giving it back.
       That waits for a write in progress and keeps every later one out, whichever task makes it. Called from every
       esp_restart() (shutdown handler), the download door, and the ui-stall guard; panics cannot call it. */
#if !BOARD_HAS_SD
    for (int i = 0; s_initing && i < 600; i++) vTaskDelay(pdMS_TO_TICKS(10));   /* up to 6 s: mount, repair, audit */
    if (s_initing) ESP_LOGW(TAG, "quiesce: storage still starting after 6 s");
#endif
    if (s_quiesced || !s_ok) return;
    s_quiesced = true;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(1500)) == pdTRUE) {
        meta_flush_locked();
        FILE *open_files[] = {s_csv, s_raw, s_pat, s_ml};
        for (size_t i = 0; i < sizeof open_files / sizeof open_files[0]; i++)
            if (open_files[i]) { fflush(open_files[i]); fsync(fileno(open_files[i])); }
        s_ok = false;
        xSemaphoreGive(s_lock);
    } else s_ok = false;
#if !BOARD_HAS_SD
    BYTE pdrv = ff_diskio_get_pdrv_wl(s_wl);
    if (pdrv != 0xFF && !ff_mutex_take(pdrv)) ESP_LOGW(TAG, "quiesce: the file system stayed busy for %d ms", CONFIG_FATFS_TIMEOUT_MS);
#endif
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
#if BOARD_HAS_SD
    esp_vfs_fat_sdcard_unmount(CONFIG_SDLOG_MOUNT_POINT, s_card);
#else
    esp_vfs_fat_spiflash_unmount_rw_wl(CONFIG_SDLOG_MOUNT_POINT, s_wl);
    s_wl = WL_INVALID_HANDLE;
#endif
#if CONFIG_OCM_NAND_LOGS
    nand_store_unmount();
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
#if CONFIG_OCM_NAND_LOGS
    s_clock_force = true;                      /* clock.txt at the next save, not up to a minute later */
#endif
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

#if BOARD_MOSAICO
/* Settings > Clock: the session's clock is now the user's; started_epoch_ms follows it (time_sync stays network-only) */
void sdlog_meta_set_clock_manual(int64_t epoch_ms, int64_t uptime_ms)
{
    if (!s_meta) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_clock_source = "manual";
    cJSON_ReplaceItemInObject(s_meta, "started_epoch_ms", cJSON_CreateNumber((double)(epoch_ms - uptime_ms)));
    meta_flush_locked();
#if CONFIG_OCM_NAND_LOGS
    s_clock_force = true;
#endif
    save_clock_locked();
    xSemaphoreGive(s_lock);
}
#endif

#if CONFIG_OCM_NAND_LOGS
const char *sdlog_drive_dir(void) { return nand_store_mounted() ? NAND_SESSIONS_DIR : SDLOG_SESSIONS_DIR; }
const char *sdlog_drives_csv(void) { return nand_store_mounted() ? NAND_LOG_DIR "/drives.csv" : SDLOG_DIR "/drives.csv"; }

int sdlog_drive_dirs(const char *dirs[2])
{
    int n = 0;
    if (nand_store_mounted()) dirs[n++] = NAND_SESSIONS_DIR;
    dirs[n++] = SDLOG_SESSIONS_DIR;
    return n;
}

bool sdlog_drive_file(const char *name, char *path, size_t n)
{
    const char *dirs[2];
    int nd = sdlog_drive_dirs(dirs);
    struct stat st;
    for (int k = 0; k < nd; k++) {
        snprintf(path, n, "%s/%.20s", dirs[k], name);
        if (stat(path, &st) == 0) return true;
    }
    snprintf(path, n, "%s/%.20s", dirs[0], name);
    return false;
}

bool sdlog_move_lock(void)
{
    if (!s_lock || s_quiesced || !s_ok) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_quiesced || !s_ok) { xSemaphoreGive(s_lock); return false; }
    return true;
}

void sdlog_move_unlock(void) { xSemaphoreGive(s_lock); }
#endif
