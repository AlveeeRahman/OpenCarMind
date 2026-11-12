/* ESP-Mosaico F1: drive logs found on the flash FAT move to the NAND drive store, once (CONFIG_OCM_NAND_LOGS).
 *
 * Why move instead of reading both stores forever: the flash FAT then holds only the state files, so its writes (each one
 * a 4 KB erase + program with the flash cache off, the C6b torn-FAT exposure) drop to the state saves; the drive store,
 * the session counter, delete and Format storage deal with one place; and the 3.75 MB the old drives take on the flash
 * FAT come back. LIST/GET and the Files app still read both stores (sdlog_drive_dirs), so nothing disappears while a move
 * runs or when one file cannot be moved.
 *
 * Every file: CRC32 of the flash copy, copy to <name>.mv on the NAND, fsync (FatFs sync + Dhara checkpoint), read the copy
 * back and compare size and CRC, rename it to <name>, and only then remove the flash copy (log_store_move_step and
 * log_store_copy_verified decide; host-tested). A reset anywhere leaves either the flash copy, or both identical copies
 * (the next boot removes the flash one), plus at most a partial .mv that the next boot deletes. Each step holds the
 * logger's lock for one 16 KB chunk at most; once a reset is announced (sdlog_quiesce_for_reset) the move stops. */
#include "sdkconfig.h"
#if CONFIG_OCM_NAND_LOGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdlog.h"
#include "nand_store.h"
#include "log_store.h"
#include "events.h"

static const char *TAG = "logmove";
#define CHUNK     (16 * 1024)
#define MAX_NAMES 512
#define CSV_MAX   (256 * 1024)

static uint8_t *s_buf;

/* CRC32 and size of a whole file, one chunk per lock; false when the file cannot be read or a reset is coming */
static bool file_crc(const char *path, uint64_t *size, uint32_t *crc)
{
    *size = 0;
    *crc = 0;
    if (!sdlog_move_lock()) return false;
    FILE *f = fopen(path, "rb");
    sdlog_move_unlock();
    if (!f) return false;
    for (;;) {
        if (!sdlog_move_lock()) return false;            /* a reset follows: leave the file open, touch nothing more */
        size_t n = fread(s_buf, 1, CHUNK, f);
        sdlog_move_unlock();
        if (!n) break;
        *crc = esp_rom_crc32_le(*crc, s_buf, n);
        *size += n;
    }
    if (!sdlog_move_lock()) return false;
    bool ok = !ferror(f);
    fclose(f);
    sdlog_move_unlock();
    return ok;
}

static bool copy_file(const char *from, const char *to)
{
    if (!sdlog_move_lock()) return false;
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    sdlog_move_unlock();
    if (!in || !out) {
        if (sdlog_move_lock()) { if (in) fclose(in); if (out) fclose(out); sdlog_move_unlock(); }
        return false;
    }
    bool ok = true;
    for (;;) {
        if (!sdlog_move_lock()) return false;
        size_t n = fread(s_buf, 1, CHUNK, in);
        bool w = !n || fwrite(s_buf, 1, n, out) == n;
        sdlog_move_unlock();
        if (!w) { ok = false; break; }
        if (!n) break;
    }
    if (!sdlog_move_lock()) return false;
    ok = !ferror(in) && ok;
    ok = fflush(out) == 0 && ok;
    ok = fsync(fileno(out)) == 0 && ok;                   /* FatFs f_sync -> CTRL_SYNC -> Dhara checkpoint */
    ok = fclose(out) == 0 && ok;
    fclose(in);
    sdlog_move_unlock();
    return ok;
}

static bool locked_remove(const char *path)
{
    if (!sdlog_move_lock()) return false;
    bool ok = remove(path) == 0;
    sdlog_move_unlock();
    return ok;
}

static bool locked_rename(const char *from, const char *to)
{
    if (!sdlog_move_lock()) return false;
    bool ok = rename(from, to) == 0;
    sdlog_move_unlock();
    return ok;
}

static int collect(const char *dir, char (*names)[24], bool (*want)(const char *))
{
    int n = 0;
    if (!sdlog_move_lock()) return -1;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d)) != NULL && n < MAX_NAMES;)
        if (strlen(e->d_name) < 24 && want(e->d_name)) snprintf(names[n++], 24, "%.23s", e->d_name);
    if (d) closedir(d);
    sdlog_move_unlock();
    return n;
}

static bool is_drive_name(const char *n) { return log_store_is_drive_file(n) && !log_store_is_move_tmp(n); }

typedef struct { int moved, dropped, conflicts, failed; uint64_t bytes; } move_stats_t;

static void move_one(const char *name, move_stats_t *ms)
{
    char from[96], to[96], tmp[100];
    snprintf(from, sizeof from, SDLOG_SESSIONS_DIR "/%.23s", name);
    snprintf(to, sizeof to, NAND_SESSIONS_DIR "/%.23s", name);
    snprintf(tmp, sizeof tmp, "%s" LOG_STORE_MOVE_SUFFIX, to);
    uint64_t fsz = 0, nsz = 0, tsz = 0;
    uint32_t fcrc = 0, ncrc = 0, tcrc = 0;
    struct stat st;
    bool on_nand = stat(to, &st) == 0;
    if (!file_crc(from, &fsz, &fcrc) || (on_nand && !file_crc(to, &nsz, &ncrc))) { ms->failed++; return; }
    switch (log_store_move_step(on_nand, nsz, ncrc, fsz, fcrc)) {
    case LS_DROP_FLASH:
        if (locked_remove(from)) ms->dropped++;
        break;
    case LS_CONFLICT:
        ms->conflicts++;
        ESP_LOGW(TAG, "%s differs on the two stores (%llu / %llu B): both kept, the NAND one is listed", name,
                 (unsigned long long)nsz, (unsigned long long)fsz);
        break;
    case LS_COPY:
        if (copy_file(from, tmp) && file_crc(tmp, &tsz, &tcrc) && log_store_copy_verified(fsz, fcrc, tsz, tcrc) &&
            locked_rename(tmp, to)) {
            if (locked_remove(from)) { ms->moved++; ms->bytes += fsz; }
        } else {
            ms->failed++;
            locked_remove(tmp);
            ESP_LOGW(TAG, "%s not moved (copy or read-back check failed): kept on the flash FAT", name);
        }
        break;
    }
}

static void move_task(void *arg)
{
    (void)arg;
    EXT_RAM_BSS_ATTR static char names[MAX_NAMES][24];
    move_stats_t ms = {0};
    char path[96];
    int n = collect(NAND_SESSIONS_DIR, names, log_store_is_move_tmp);       /* copies a reset cut short */
    for (int i = 0; i < n; i++) { snprintf(path, sizeof path, NAND_SESSIONS_DIR "/%.23s", names[i]); locked_remove(path); }
    n = collect(SDLOG_SESSIONS_DIR, names, is_drive_name);
    if (n > 0) ESP_LOGI(TAG, "moving %d drive-log files from the flash FAT to the NAND", n);
    for (int i = 0; i < n; i++) {
        move_one(names[i], &ms);
        if (!sdlog_move_lock()) { ESP_LOGW(TAG, "move stopped: a reset is coming"); vTaskDelete(NULL); }
        sdlog_move_unlock();
    }
    if (n > 0) {
        ESP_LOGI(TAG, "move done: %d files (%llu KB) moved, %d duplicates removed, %d conflicts kept, %d failed", ms.moved,
                 (unsigned long long)(ms.bytes / 1024), ms.dropped, ms.conflicts, ms.failed);
        sys_event(ms.failed || ms.conflicts ? EV_WARN : EV_INFO, "Drive logs moved to the NAND: %d files, %llu KB%s",
                  ms.moved + ms.dropped, (unsigned long long)(ms.bytes / 1024),
                  ms.failed || ms.conflicts ? " (some kept on the flash FAT)" : "");
    }
    free(s_buf);
    s_buf = NULL;
    vTaskDelete(NULL);
}

void log_move_start(void)
{
    if (!nand_store_mounted() || s_buf) return;
    s_buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_buf) return;
    /* internal-RAM stack: the task reads and deletes on the flash FAT, where a write turns the flash cache off */
    if (xTaskCreate(move_task, "logmove", 4096, NULL, 1, NULL) != pdPASS) { free(s_buf); s_buf = NULL; }
}

static long read_all(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    if (more) return -2;
    buf[n] = 0;
    return (long)n;
}

/* drives.csv: merged into the NAND's, synchronously at boot (sdlog_init, before s_ok: the door quiesce waits for it).
   The NAND keeps the merged file under drives.csv.mv until it was read back; a reset between removing the old NAND file and
   the rename leaves only the verified .mv, which is taken as the file at the next boot. */
void log_move_drives_csv(void)
{
    static const char *nor = SDLOG_DIR "/drives.csv", *nand = NAND_LOG_DIR "/drives.csv", *mv = NAND_LOG_DIR "/drives.csv.mv";
    struct stat st;
    bool have_nand = stat(nand, &st) == 0, have_mv = stat(mv, &st) == 0;
    if (have_mv && !have_nand) { rename(mv, nand); have_nand = true; have_mv = false; }
    if (have_mv) remove(mv);
    if (stat(nor, &st) != 0) return;
    char *a = heap_caps_malloc(CSV_MAX, MALLOC_CAP_SPIRAM), *b = heap_caps_malloc(CSV_MAX, MALLOC_CAP_SPIRAM);
    char *out = heap_caps_malloc(2 * CSV_MAX, MALLOC_CAP_SPIRAM);
    long la = a ? read_all(nor, a, CSV_MAX) : -1, lb = have_nand && b ? read_all(nand, b, CSV_MAX) : 0;
    if (b && lb == 0) b[0] = 0;
    long lo = la >= 0 && lb >= 0 && out ? log_store_merge_drives_csv(a, b, out, 2 * CSV_MAX) : -1;
    bool ok = false;
    if (lo >= 0) {
        FILE *f = fopen(mv, "wb");
        ok = f && fwrite(out, 1, (size_t)lo, f) == (size_t)lo;
        if (f) { ok = fflush(f) == 0 && ok; ok = fsync(fileno(f)) == 0 && ok; ok = fclose(f) == 0 && ok; }
        long back = ok ? read_all(mv, a, CSV_MAX) : -1;  /* a is free now: the read-back */
        ok = ok && back == lo && !memcmp(a, out, (size_t)lo);
        if (ok && have_nand) ok = remove(nand) == 0;
        if (ok) ok = rename(mv, nand) == 0;
        if (ok) remove(nor);
        else remove(mv);
    }
    ESP_LOGI(TAG, "drives.csv from the flash FAT (%ld B) merged into the NAND's (%ld B): %ld B, %s", la, lb, lo,
             ok ? "flash copy removed" : "NOT merged, flash copy kept");
    free(a);
    free(b);
    free(out);
}
#endif
