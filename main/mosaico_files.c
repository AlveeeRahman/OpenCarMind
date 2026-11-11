#include "mosaico_files.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdlog.h"
#if CONFIG_OCM_NAND_LOGS
#include "nand_store.h"
#endif

static const char *TAG = "files";
static SemaphoreHandle_t s_mx;
EXT_RAM_BSS_ATTR static mf_list_t s_list, s_work;
EXT_RAM_BSS_ATTR static mf_detail_t s_det, s_det_work;
EXT_RAM_BSS_ATTR static char s_buf[4096];

static void lock(void) { if (!s_mx) s_mx = xSemaphoreCreateMutex(); xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

void mosaico_files_get_list(mf_list_t *out) { lock(); *out = s_list; unlock(); }
void mosaico_files_get_detail(mf_detail_t *out) { lock(); *out = s_det; unlock(); }

static void publish_busy(bool busy)
{
    lock();
    s_list.busy = busy;
    if (!busy) { uint32_t g = s_list.gen + 1; char note[64]; memcpy(note, s_list.note, sizeof note); s_list = s_work; s_list.gen = g; memcpy(s_list.note, note, sizeof note); }
    unlock();
}

/* value after "key": in the first 4 KB of the session JSON (cJSON prints these near the top) */
static double json_num(const char *j, const char *key, double dflt)
{
    const char *p = strstr(j, key);
    if (!p) return dflt;
    p = strchr(p + strlen(key), ':');
    return p ? strtod(p + 1, NULL) : dflt;
}

static void json_str(const char *j, const char *obj, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const char *p = obj ? strstr(j, obj) : j;
    if (!p || !(p = strstr(p, key)) || !(p = strchr(p + strlen(key), ':')) || !(p = strchr(p, '"'))) return;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
}

static size_t read_head(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = 0; return 0; }
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return r;
}

static int cmp_desc(const void *a, const void *b)
{
    uint32_t x = ((const mf_session_t *)a)->id, y = ((const mf_session_t *)b)->id;
    return x < y ? 1 : x > y ? -1 : 0;
}

void mosaico_files_scan(void)
{
    publish_busy(true);
    memset(&s_work, 0, sizeof s_work);
    uint64_t total = 0, free_b = 0;
    if (sdlog_ok() && esp_vfs_fat_info(CONFIG_SDLOG_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        s_work.total_kb = (uint32_t)(total / 1024); s_work.free_kb = (uint32_t)(free_b / 1024);
    }
#if CONFIG_OCM_NAND_LOGS
    if (sdlog_ok() && nand_store_info(&total, &free_b)) {
        s_work.nand = true;
        s_work.flash_total_kb = s_work.total_kb; s_work.flash_free_kb = s_work.free_kb;
        s_work.total_kb = (uint32_t)(total / 1024); s_work.free_kb = (uint32_t)(free_b / 1024);
    }
    const char *dirs[2];
    int nd = sdlog_ok() ? sdlog_drive_dirs(dirs) : 0;
    if (!nd) { publish_busy(false); return; }
#else
    DIR *d = sdlog_ok() ? opendir(SDLOG_SESSIONS_DIR) : NULL;
    if (!d) { publish_busy(false); return; }
#endif
    /* every session id with its file count and bytes; the newest MF_MAX are kept */
    EXT_RAM_BSS_ATTR static mf_session_t all[256];
    int n = 0;
    uint32_t cur = sdlog_session_id();
#if CONFIG_OCM_NAND_LOGS
    for (int dk = 0; dk < nd; dk++) {                  /* the NAND, then drives still on the flash FAT */
    DIR *d = opendir(dirs[dk]);
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        if (!session_name_valid(e->d_name)) continue;
        char other[96];
        struct stat ost;
        snprintf(other, sizeof other, "%s/%.20s", dirs[0], e->d_name);
        if (dk > 0 && stat(other, &ost) == 0) continue;  /* on both stores: counted once */
#else
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (!session_name_valid(e->d_name)) continue;
#endif
        uint32_t id = (uint32_t)strtoul(e->d_name + 1, NULL, 10);
        int k = 0;
        while (k < n && all[k].id != id) k++;
        if (k == n) {
            if (n == 256) continue;
            memset(&all[n], 0, sizeof all[n]);
            all[n].id = id; all[n].current = id == cur;
            n++;
        }
        char path[96];
        struct stat st;
#if CONFIG_OCM_NAND_LOGS
        snprintf(path, sizeof path, "%s/%.20s", dirs[dk], e->d_name);
        if (stat(path, &st) == 0) all[k].bytes += (uint32_t)st.st_size;
        all[k].nfiles++;
    }
    if (d) closedir(d);
    }
#else
        snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%.20s", e->d_name);
        if (stat(path, &st) == 0) all[k].bytes += (uint32_t)st.st_size;
        all[k].nfiles++;
    }
    closedir(d);
#endif
    qsort(all, n, sizeof all[0], cmp_desc);
    s_work.total = n;
    s_work.n = n < MF_MAX ? n : MF_MAX;
    for (int i = 0; i < s_work.n; i++) {
        mf_session_t *s = &s_work.s[i];
        *s = all[i];
        char path[96], name[24];
        snprintf(name, sizeof name, "s%06lu.json", (unsigned long)s->id);
#if CONFIG_OCM_NAND_LOGS
        sdlog_drive_file(name, path, sizeof path);
#else
        snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%s", name);
#endif
        if (read_head(path, s_buf, sizeof s_buf)) {
            s->rows = (uint32_t)json_num(s_buf, "\"rows\"", 0);
            s->start_epoch_ms = (int64_t)json_num(s_buf, "\"started_epoch_ms\"", 0);
            s->model_year = (int16_t)json_num(s_buf, "\"model_year\"", 0);
        }
    }
    s_work.ok = true;
    publish_busy(false);
    ESP_LOGI(TAG, "%d drive logs, %lu of %lu KB free", n, (unsigned long)s_work.free_kb, (unsigned long)s_work.total_kb);
}

/* column index of name in the CSV header */
static int column(const char *header, const char *name)
{
    int col = 0;
    size_t L = strlen(name);
    for (const char *p = header; *p && *p != '\n'; col++) {
        if (!strncmp(p, name, L) && (p[L] == ',' || p[L] == '\n' || p[L] == '\r')) return col;
        const char *c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
    return -1;
}

static const char *field(const char *line, int col)
{
    for (int i = 0; i < col && line; i++) { line = strchr(line, ','); if (line) line++; }
    return line;
}

void mosaico_files_detail(uint32_t id)
{
    memset(&s_det_work, 0, sizeof s_det_work);
    mf_detail_t *o = &s_det_work;
    o->id = id; o->km = o->fuel_l = -1;
    o->current = id == sdlog_session_id();
    char path[96], name[24];
    snprintf(name, sizeof name, "s%06lu.json", (unsigned long)id);
#if CONFIG_OCM_NAND_LOGS
    sdlog_drive_file(name, path, sizeof path);
#else
    snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%s", name);
#endif
    if (read_head(path, s_buf, sizeof s_buf)) {
        o->rows = (uint32_t)json_num(s_buf, "\"rows\"", 0);
        o->start_epoch_ms = (int64_t)json_num(s_buf, "\"started_epoch_ms\"", 0);
        o->model_year = (int16_t)json_num(s_buf, "\"model_year\"", 0);
        json_str(s_buf, "\"vehicle\"", "\"protocol\"", o->protocol, sizeof o->protocol);
        json_str(s_buf, "\"adapter\"", "\"name\"", o->adapter, sizeof o->adapter);
    }
    char pre[8];
    snprintf(pre, sizeof pre, "s%06lu", (unsigned long)id);
#if CONFIG_OCM_NAND_LOGS
    const char *dirs[2];
    int nd = sdlog_drive_dirs(dirs);
    for (int dk = 0; dk < nd; dk++) {
    DIR *d = opendir(dirs[dk]);
    for (struct dirent *e; d && (e = readdir(d)) != NULL && o->nf < MF_FILES;) {
        if (!session_name_valid(e->d_name) || strncmp(e->d_name, pre, 7)) continue;
        struct stat st;
        snprintf(path, sizeof path, "%s/%.20s", dirs[0], e->d_name);
        if (dk > 0 && stat(path, &st) == 0) continue;     /* on both stores: once */
        snprintf(path, sizeof path, "%s/%.20s", dirs[dk], e->d_name);
#else
    DIR *d = opendir(SDLOG_SESSIONS_DIR);
    for (struct dirent *e; d && (e = readdir(d)) != NULL && o->nf < MF_FILES;) {
        if (!session_name_valid(e->d_name) || strncmp(e->d_name, pre, 7)) continue;
        struct stat st;
        snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%.20s", e->d_name);
#endif
        snprintf(o->f[o->nf].name, sizeof o->f[0].name, "%.20s", e->d_name);
        o->f[o->nf].bytes = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
        o->nf++;
    }
    if (d) closedir(d);
#if CONFIG_OCM_NAND_LOGS
    }
#endif
    /* start, duration and distance from the CSV: header + first row, and the last row (never the file being written) */
    snprintf(name, sizeof name, "s%06lu.csv", (unsigned long)id);
#if CONFIG_OCM_NAND_LOGS
    sdlog_drive_file(name, path, sizeof path);
#else
    snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%s", name);
#endif
    FILE *f = o->current ? NULL : fopen(path, "r");
    if (f) {
        size_t r = fread(s_buf, 1, sizeof s_buf - 1, f);
        s_buf[r] = 0;
        int c_up = column(s_buf, "uptime_ms"), c_ep = column(s_buf, "epoch_ms"), c_km = column(s_buf, "trip_km"),
            c_fl = column(s_buf, "trip_fuel_l");
        const char *row1 = strchr(s_buf, '\n');
        if (row1 && row1[1]) {
            row1++;
            const char *v = field(row1, c_up);
            if (v) o->first_up_ms = strtoll(v, NULL, 10);
            v = field(row1, c_ep);
            int64_t ep = v ? strtoll(v, NULL, 10) : 0;
            if (ep > 1600000000000LL) o->start_epoch_ms = ep;
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            long from = size > (long)sizeof s_buf - 1 ? size - (long)sizeof s_buf + 1 : 0;
            fseek(f, from, SEEK_SET);
            r = fread(s_buf, 1, sizeof s_buf - 1, f);
            s_buf[r] = 0;
            while (r && (s_buf[r - 1] == '\n' || s_buf[r - 1] == '\r')) s_buf[--r] = 0;
            char *last = strrchr(s_buf, '\n');
            last = last ? last + 1 : s_buf;
            if ((v = field(last, c_up))) o->last_up_ms = strtoll(v, NULL, 10);
            if (c_km >= 0 && (v = field(last, c_km)) && *v && *v != ',') o->km = strtof(v, NULL);
            if (c_fl >= 0 && (v = field(last, c_fl)) && *v && *v != ',') o->fuel_l = strtof(v, NULL);
        }
        fclose(f);
    }
    o->ok = true;
    lock();
    uint32_t g = s_det.gen + 1;
    s_det = s_det_work;
    s_det.gen = g;
    unlock();
}

void mosaico_files_note(const char *note)
{
    lock();
    snprintf(s_list.note, sizeof s_list.note, "%s", note);
    unlock();
}

void mosaico_files_delete(uint32_t id)
{
    char note[64];
    if (id) {
        int files = sdlog_delete_session(id);
        if (files < 0) snprintf(note, sizeof note, "s%06lu not deleted (being written or no storage)", (unsigned long)id);
        else snprintf(note, sizeof note, "s%06lu deleted (%d files)", (unsigned long)id, files);
    } else {
        int sessions = 0, files = sdlog_delete_all_sessions(&sessions);
        if (files >= 0) remove(SDLOG_DRIVES_CSV);           /* the drive history goes with "delete all" only */
#if CONFIG_OCM_NAND_LOGS
        if (files >= 0) remove(SDLOG_DIR "/drives.csv");    /* and an old one not merged into the NAND's yet */
#endif
        if (files < 0) snprintf(note, sizeof note, "nothing deleted (no storage)");
        else snprintf(note, sizeof note, "%d drive logs deleted (%d files), current kept", sessions, files);
    }
    lock();
    memcpy(s_list.note, note, sizeof note);
    unlock();
    mosaico_files_scan();
}
