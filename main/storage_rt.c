/* Storage reset-injection test (CONFIG_OCM_STORAGE_RESET_TEST, test builds only; default n).
 *
 * Every boot: check what the previous cycle left on the flash FAT, then run a writer that keeps the volume busy the
 * way the logger and the learned-state saves do (appends with fsync, a 36 KB tmp+fsync+remove+rename save, small files
 * created and deleted) and reset the board at a random moment, mostly inside a write: esp_restart(), abort() (panic and
 * core dump), the download door's ROM system reset, or (stall variant) a frozen UI so the ui-stall guard fires.
 * Counters live in NVS (namespace "srt"), so they survive every kind of reset; a new firmware image starts a new run.
 *
 * A volume loss is what the owner would see as "the storage is gone": the FAT does not mount, a 16 KB probe file cannot
 * be written and read back, 0 KB free, or more than 512 KB used beyond what the files need. After a loss the test erases
 * the partition and restarts, so the next cycle starts on a fresh volume and losses are counted one by one.
 * File damage (the CRC'd state file unreadable after it was saved, or a bad line inside the append log) is counted apart. */
#include "storage_rt.h"
#include "sdkconfig.h"
#if CONFIG_OCM_STORAGE_RESET_TEST
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "sdlog.h"
#include "rev_state.h"

static const char *TAG = "srt";
#ifdef CONFIG_OCM_STORAGE_RESET_TEST_STALL
#define RT_STALL 1
#else
#define RT_STALL 0
#endif

#define RT_DIR      SDLOG_DIR "/rt"
#define RT_STATE    RT_DIR "/state.bin"
#define RT_LOG      RT_DIR "/log.txt"
#define RT_PROBE    RT_DIR "/probe.bin"
#define RT_MAGIC    0x31545253u          /* "SRT1" */
#define RT_STATE_SZ (36 * 1024)          /* a large learned state: 9 clusters allocated and freed on every save */

enum { M_RESTART, M_ABORT, M_ROMRESET, M_STALL, M_N };
static const char *const MODE_NAME[M_N] = {"esp_restart", "abort", "rom_reset", "ui_stall"};

typedef struct {
    uint8_t  elf[8];                     /* first bytes of the image's ELF SHA-256: a new image starts a new run */
    uint32_t cycle, target, losses, damage, state_seen, last_mode, log_seq;
    uint32_t by_mode[M_N], loss_by_mode[M_N];
} rt_nvs_t;

RTC_NOINIT_ATTR static uint32_t s_rtc_magic, s_rtc_saves, s_rtc_op;   /* survive software resets and panics */
static rt_nvs_t s_st;
static uint8_t *s_blob;

static void st_save(void)
{
    nvs_handle_t h;
    if (nvs_open("srt", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "st", &s_st, sizeof s_st);
    nvs_commit(h);
    nvs_close(h);
}

static void st_load(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    nvs_handle_t h;
    size_t n = sizeof s_st;
    bool ok = false;
    if (nvs_open("srt", NVS_READWRITE, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "st", &s_st, &n) == ESP_OK && n == sizeof s_st;
        nvs_close(h);
    }
    if (!ok || memcmp(s_st.elf, d->app_elf_sha256, sizeof s_st.elf) != 0) {
        memset(&s_st, 0, sizeof s_st);
        memcpy(s_st.elf, d->app_elf_sha256, sizeof s_st.elf);
        s_st.target = CONFIG_OCM_STORAGE_RESET_TEST_CYCLES;
        s_rtc_magic = 0;
    }
}

static int s_garbage;                     /* directory entries a torn directory sector left: odd names or sizes */

static uint64_t tree_need(const char *dir, uint32_t clu, int depth)
{
    uint64_t need = clu;                 /* the directory's own cluster */
    DIR *d = opendir(dir);
    if (!d) return need;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        char p[288];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        bool odd = false;
        for (const char *c = e->d_name; *c; c++) odd |= (unsigned char)*c < 0x20 || (unsigned char)*c >= 0x7F;
        if (stat(p, &st) != 0 || odd || (!S_ISDIR(st.st_mode) && st.st_size > 4 * 1024 * 1024)) { s_garbage++; continue; }
        if (S_ISDIR(st.st_mode)) { if (depth < 4) need += tree_need(p, clu, depth + 1); }
        else need += ((uint64_t)st.st_size + clu - 1) / clu * clu;
    }
    closedir(d);
    return need;
}

static bool probe(void)
{
    static uint8_t buf[4096];
    bool ok = true;
    FILE *f = fopen(RT_PROBE, "wb");
    if (!f) return false;
    for (int i = 0; i < 4 && ok; i++) { memset(buf, 0x5A ^ i, sizeof buf); ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf; }
    ok = fflush(f) == 0 && ok;
    fsync(fileno(f));
    ok = fclose(f) == 0 && ok;
    f = ok ? fopen(RT_PROBE, "rb") : NULL;
    for (int i = 0; f && i < 4 && ok; i++) {
        ok = fread(buf, 1, sizeof buf, f) == sizeof buf;
        for (size_t k = 0; ok && k < sizeof buf; k++) ok = buf[k] == (uint8_t)(0x5A ^ i);
    }
    if (f) fclose(f);
    else ok = false;
    unlink(RT_PROBE);
    return ok;
}

/* Lines "<seq> <seq xor 0xA5A5A5A5>": a line with a bad check is damage, except the very last one (an append torn by
   the reset is expected). */
static int log_bad_lines(uint32_t *last_seq)
{
    FILE *f = fopen(RT_LOG, "r");
    if (!f) return 0;
    char line[64];
    int bad = 0, pending = 0;
    while (fgets(line, sizeof line, f)) {
        bad += pending;
        unsigned long a = 0, b = 0;
        pending = !(sscanf(line, "%lu %lu", &a, &b) == 2 && (uint32_t)b == ((uint32_t)a ^ 0xA5A5A5A5u) && strchr(line, '\n'));
        if (!pending) *last_seq = (uint32_t)a;
    }
    fclose(f);
    return bad;
}

static void wipe_and_restart(const char *why)
{
    ESP_LOGE(TAG, "SRT LOSS cycle %lu after %s: %s; erasing the storage partition for the next cycle",
             (unsigned long)s_st.cycle, MODE_NAME[s_st.last_mode], why);
    sdlog_prepare_sleep();
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (p) esp_partition_erase_range(p, 0, p->size);
    s_rtc_magic = 0;
    s_st.state_seen = 0;
    st_save();
    esp_rom_software_reset_system();
}

static void check_previous(void)
{
    char why[96] = "";
    bool mounted = sdlog_ok();
    uint64_t total = 0, freeb = 0, need = 0;
    bool probe_ok = false, state_ok = false;
    int bad = 0;
    if (mounted && esp_vfs_fat_info(CONFIG_SDLOG_MOUNT_POINT, &total, &freeb) == ESP_OK) {
        mkdir(RT_DIR, 0775);
        need = tree_need(CONFIG_SDLOG_MOUNT_POINT, 4096, 0);
        probe_ok = probe();
        state_ok = rev_blob_load(RT_STATE, RT_MAGIC, s_blob, RT_STATE_SZ);
        uint32_t last = 0;
        bad = log_bad_lines(&last);
        if (last > s_st.log_seq) s_st.log_seq = last;
    }
    uint64_t used = total - freeb, lost = used > need ? used - need : 0;
    if (!mounted) snprintf(why, sizeof why, "the FAT did not mount");
    else if (!total) snprintf(why, sizeof why, "no volume info");
    else if (!freeb) snprintf(why, sizeof why, "0 KB free (%llu KB used, files need %llu KB)", used / 1024, need / 1024);
    else if (lost > 512 * 1024) snprintf(why, sizeof why, "%llu KB lost clusters", lost / 1024);
    else if (!probe_ok) snprintf(why, sizeof why, "the 16 KB probe write/read failed");
    bool saved_before = s_rtc_magic == RT_MAGIC && s_rtc_saves > 0;
    bool damaged = (!state_ok && (s_st.state_seen || saved_before)) || bad > 0 || s_garbage > 0;
    ESP_LOGW(TAG, "SRT cycle %lu/%lu after %s (op %lu): mounted %d, %llu of %llu KB free, lost %llu KB, probe %d, state %d%s, "
             "log bad %d, odd dir entries %d -> %s",
             (unsigned long)s_st.cycle, (unsigned long)s_st.target, MODE_NAME[s_st.last_mode], (unsigned long)s_rtc_op, mounted,
             freeb / 1024, total / 1024, lost / 1024, probe_ok, state_ok, saved_before ? " (saved last boot)" : "", bad, s_garbage,
             why[0] ? "VOLUME LOSS" : damaged ? "file damage" : "ok");
    if (state_ok) s_st.state_seen = 1;
    if (damaged && !why[0]) s_st.damage++;
    if (why[0]) {
        s_st.losses++;
        s_st.loss_by_mode[s_st.last_mode]++;
        wipe_and_restart(why);
    }
    if (bad) unlink(RT_LOG);                          /* start the log again, so one damage is counted once */
}

static void do_reset(int mode)
{
    switch (mode) {
    case M_RESTART: esp_restart(); break;
    case M_ABORT: abort(); break;
    default:
        sdlog_quiesce_for_reset();                     /* what the download door does before its ROM reset */
        esp_rom_software_reset_system();
    }
}

static void timer_cb(void *arg) { do_reset((int)(intptr_t)arg); }

static void stall_task(void *arg)
{
    (void)arg;
    lvgl_port_lock(0);                                 /* the LVGL task blocks: no ui tick, as when the panel hung on 2026-11-11 */
    for (;;) vTaskDelay(portMAX_DELAY);
}

static void writer_task(void *arg)
{
    (void)arg;
    uint32_t seq = s_st.log_seq + 1, it = 0;
    for (;;) {
        it++;
        s_rtc_op = 1;
        FILE *f = fopen(RT_LOG, "a");
        for (int i = 0; f && i < 3; i++) fprintf(f, "%lu %lu\n", (unsigned long)seq, (unsigned long)(seq ^ 0xA5A5A5A5u)), seq++;
        if (f) { fflush(f); fsync(fileno(f)); fclose(f); }
        if (it % 4 == 0) {
            s_rtc_op = 2;
            memset(s_blob, (uint8_t)it, RT_STATE_SZ);
            if (rev_blob_save(RT_STATE, RT_MAGIC, s_blob, RT_STATE_SZ)) { s_rtc_magic = RT_MAGIC; s_rtc_saves++; }
        }
        if (it % 3 == 0) {
            s_rtc_op = 3;
            char p[48];
            snprintf(p, sizeof p, RT_DIR "/f%lu.bin", (unsigned long)(it / 3 % 3));
            unlink(p);
            f = fopen(p, "wb");
            size_t n = 1024 + esp_random() % 6144;
            for (size_t k = 0; f && k < n; k += 256) fwrite(s_blob, 1, 256, f);
            if (f) fclose(f);
        }
        struct stat st;
        if (stat(RT_LOG, &st) == 0 && st.st_size > 48 * 1024) { s_rtc_op = 4; unlink(RT_LOG); }
        s_rtc_op = 0;
        vTaskDelay(1);
    }
}

void storage_rt_run(void)
{
    s_blob = heap_caps_malloc(RT_STATE_SZ, MALLOC_CAP_SPIRAM);
    if (!s_blob) return;
    st_load();
    if (s_st.cycle > 0) check_previous();
    else ESP_LOGW(TAG, "SRT new run: %lu cycles, %s", (unsigned long)s_st.target,
                  RT_STALL ? "ui-stall variant" : "random esp_restart / abort / ROM reset");
    if (s_st.cycle >= s_st.target) {
        ESP_LOGW(TAG, "SRT DONE %lu cycles: %lu volume losses (esp_restart %lu/%lu, abort %lu/%lu, rom_reset %lu/%lu, "
                 "ui_stall %lu/%lu), %lu file damages",
                 (unsigned long)s_st.cycle, (unsigned long)s_st.losses,
                 (unsigned long)s_st.loss_by_mode[0], (unsigned long)s_st.by_mode[0], (unsigned long)s_st.loss_by_mode[1],
                 (unsigned long)s_st.by_mode[1], (unsigned long)s_st.loss_by_mode[2], (unsigned long)s_st.by_mode[2],
                 (unsigned long)s_st.loss_by_mode[3], (unsigned long)s_st.by_mode[3], (unsigned long)s_st.damage);
        return;
    }
    s_rtc_magic = RT_MAGIC;
    s_rtc_saves = 0;
    int mode = RT_STALL ? M_STALL : (int)(esp_random() % 3);
    s_st.cycle++;
    s_st.last_mode = (uint32_t)mode;
    s_st.by_mode[mode]++;
    st_save();
    xTaskCreatePinnedToCore(writer_task, "srt_wr", 4096, NULL, 4, NULL, 1);
    if (mode == M_STALL) {
        xTaskCreatePinnedToCore(stall_task, "srt_stall", 2048, NULL, 6, NULL, 0);
        ESP_LOGW(TAG, "SRT cycle %lu: UI frozen, waiting for the ui-stall guard", (unsigned long)s_st.cycle);
        return;
    }
    uint32_t ms = 300 + esp_random() % 2700;
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = timer_cb, .arg = (void *)(intptr_t)mode, .name = "srt" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (uint64_t)ms * 1000);
    ESP_LOGW(TAG, "SRT cycle %lu: %s in %lu ms", (unsigned long)s_st.cycle, MODE_NAME[mode], (unsigned long)ms);
}
#else
void storage_rt_run(void) {}
#endif
