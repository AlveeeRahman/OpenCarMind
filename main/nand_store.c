/* ESP-Mosaico SPI NAND drive store: see nand_store.h. */
#include "nand_store.h"
#if CONFIG_OCM_NAND_LOGS
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_impl.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/pmu_struct.h"
#include "spi_nand_flash.h"
#include "nand_diag_api.h"
#include "nand_private/nand_impl_wrap.h"
#include "esp_vfs_fat_nand.h"
#include "diskio_nand.h"
#include "fat_mirror.h"
#include "events.h"

static const char *TAG = "nand";

#ifdef CONFIG_OCM_NAND_QIO
#define NAND_QIO 1
#else
#define NAND_QIO 0
#endif

#define NAND_HOST      SPI3_HOST
#define PIN_CLK        GPIO_NUM_20     /* NAND_CLK = SD_D0 */
#define PIN_MOSI       GPIO_NUM_21     /* NAND_D   = SD_D1, chip SI/SIO0 */
#define PIN_MISO       GPIO_NUM_22     /* NAND_Q   = SD_D2, chip SO/SIO1 */
#define PIN_CS         GPIO_NUM_23     /* NAND_CS  = SD_D3, 10 k pull-up R15 */
#define PIN_HD         GPIO_NUM_24     /* NAND_HD  = SD_CLK, chip HOLD#/SIO3 */
#define PIN_WP         GPIO_NUM_25     /* NAND_WP  = SD_CMD, chip WP#/SIO2 */
#define AU_BYTES       (16 * 1024)     /* 8 sectors of 2 KB: ~7.5k clusters on ~120 MB = FAT16, 15 KB per FAT copy */
#define NVS_NS         "ocm_nand"
#define NVS_KEY_FMT    "fmt"           /* 1 while a format runs: an interrupted format is redone, the chip held no logs */

static spi_device_handle_t s_spi;
static spi_nand_flash_device_t *s_dev;
static volatile nand_state_t s_state = NAND_ABSENT;
static SemaphoreHandle_t s_nlock;      /* held during mount and format; nand_store_quiesce takes it and keeps it */
static volatile bool s_quiesced;
static uint8_t s_mi, s_di;
static uint32_t s_blocks, s_bad;
static char s_desc[96] = "no SPI NAND";

nand_state_t nand_store_state(void) { return s_state; }
const char *nand_store_describe(void) { return s_desc; }

static void fmt_flag(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_FMT, v);
    nvs_commit(h);
    nvs_close(h);
}

static bool fmt_was_interrupted(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, NVS_KEY_FMT, &v); nvs_close(h); }
    return v == 1;
}

/* READ ID (9Fh) with one address/dummy byte returns manufacturer, device: C8 91 for the GD5F1GM7UE (the driver's table,
   nand_gigadevice.c, takes 0x91 and 0x81 as the 1 Gbit GD5F1GM7). */
static void read_id(void)
{
    spi_transaction_ext_t t = {
        .base = { .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_USE_RXDATA, .cmd = 0x9F, .addr = 0,
                  .rxlength = 16 },
        .command_bits = 8, .address_bits = 8,
    };
    if (spi_device_transmit(s_spi, &t.base) == ESP_OK) { s_mi = t.base.rx_data[0]; s_di = t.base.rx_data[1]; }
}

static esp_err_t bus_init(void)
{
    /* GPIO20-25 sit in the VDDPST_SD pad domain, fed from VDDPST_2 (3.3 V) or the 1.8 V LDO (datasheet 2.5.2). The reset
       value 0 is the 3.3 V source: sdmmc_ll_switch_io_power_control_src() sets 1 only for the UHS-I 1.8 V switch. Logged
       so the board check sees it (the register is "need_des" in IDF 6.1: UNVERIFIED on the board). */
    ESP_LOGI(TAG, "SDIO pad supply select %lu (0 = VDDPST_2, 3.3 V)", (unsigned long)PMU.vdd_sdio_cfg.sdio_sw_power_sel);
#if !NAND_QIO
    /* single-line SPI: WP# and HOLD# are not data lines and have no pull-ups on the board; held high (HOLD# low pauses the
       chip, WP# low write-protects the block-lock bits) */
    gpio_config_t io = { .pin_bit_mask = BIT64(PIN_WP) | BIT64(PIN_HD), .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    gpio_set_level(PIN_WP, 1);
    gpio_set_level(PIN_HD, 1);
#endif
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_CLK, .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
#if NAND_QIO
        .quadwp_io_num = PIN_WP, .quadhd_io_num = PIN_HD,
#else
        .quadwp_io_num = -1, .quadhd_io_num = -1,
#endif
        .max_transfer_sz = 4096 + 64,
    };
    esp_err_t err = spi_bus_initialize(NAND_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t dev = {
        .clock_speed_hz = CONFIG_OCM_NAND_SPI_MHZ * 1000 * 1000, .mode = 0, .spics_io_num = PIN_CS, .queue_size = 4,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    return spi_bus_add_device(NAND_HOST, &dev, &s_spi);
}

/* ---------- FAT checks before the mount: the flash FAT's fat_mirror rules, through the Dhara sector map ----------
   On Dhara a sector write is atomic and a reset rolls the map back to the last sync (dhara README "Data integrity";
   map.c returns 0xFF for a sector never written). So no sector is ever half erased, but the rollback point can fall
   between FatFs's FAT 1 and FAT 2 writes (both complete, different: fat_mirror's "FAT 2 agrees with the directory" rule)
   or after a FAT chain was allocated and before its directory entry (orphans: fat_reclaim). */
static bool sec_read(uint32_t lba, uint8_t *buf, void *ctx) { (void)ctx; return spi_nand_flash_read_page(s_dev, buf, lba) == ESP_OK; }
static bool sec_write(uint32_t lba, const uint8_t *buf, void *ctx) { (void)ctx; return spi_nand_flash_write_page(s_dev, buf, lba) == ESP_OK; }

typedef enum { PRE_OK, PRE_NO_FAT, PRE_BOTH_BAD, PRE_ERROR } pre_t;
static int s_repairs, s_scrubbed;
static uint32_t s_reclaimed_kb;

static void reclaim(const fat_geom_t *g, uint8_t *sec)
{
    size_t ss = g->sector_size, n = (size_t)g->fat_sectors * ss;
    if (n > 64 * 1024) return;
    uint8_t *fat = heap_caps_malloc(n, MALLOC_CAP_SPIRAM), *orig = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    uint8_t *mark = heap_caps_calloc(1, FAT_RECLAIM_MARK_BYTES(g), MALLOC_CAP_SPIRAM);
    bool ok = fat && orig && mark;
    for (uint32_t i = 0; ok && i < g->fat_sectors; i++) ok = sec_read(g->fat_start + i, fat + i * ss, NULL);
    int freed = ok ? (memcpy(orig, fat, n), fat_reclaim(g, fat, sec_read, sec_write, NULL, sec, mark, &s_scrubbed)) : -1;
    if (s_scrubbed) ESP_LOGE(TAG, "NAND: %d broken directory entr%s marked deleted", s_scrubbed, s_scrubbed == 1 ? "y" : "ies");
    if (freed < 0) ESP_LOGW(TAG, "NAND: directory tree not clean: lost clusters left as they are");
    for (int copy = 0; freed > 0 && copy < g->n_fats; copy++)           /* FAT 1 first, as FatFs writes */
        for (uint32_t i = 0; i < g->fat_sectors; i++)
            if (memcmp(fat + i * ss, orig + i * ss, ss) && !sec_write(g->fat_start + copy * g->fat_sectors + i, fat + i * ss, NULL)) {
                freed = -1;
                break;
            }
    if (freed > 0) {
        s_reclaimed_kb = (uint32_t)((uint64_t)freed * g->sec_per_clus * ss / 1024);
        ESP_LOGW(TAG, "NAND: %d lost cluster%s (%lu KB) from interrupted writes freed", freed, freed == 1 ? "" : "s",
                 (unsigned long)s_reclaimed_kb);
    }
    free(fat);
    free(orig);
    free(mark);
}

static pre_t preflight(void)
{
    uint32_t ss = 0;
    spi_nand_flash_get_page_size(s_dev, &ss);
    uint8_t *a = heap_caps_malloc(ss, MALLOC_CAP_SPIRAM), *b = heap_caps_malloc(ss, MALLOC_CAP_SPIRAM);
    uint8_t *c = heap_caps_malloc(ss, MALLOC_CAP_SPIRAM);
    pre_t r = PRE_OK;
    fat_geom_t g;
    if (!a || !b || !c || !sec_read(0, a, NULL)) r = PRE_ERROR;
    else if (!fat_geom_parse(a, ss, sec_read, NULL, c, &g)) r = PRE_NO_FAT;
    else if (g.sector_size != ss) ESP_LOGW(TAG, "NAND: FAT sector size %lu, page %lu: FAT copies not checked",
                                           (unsigned long)g.sector_size, (unsigned long)ss);
    else {
        for (uint32_t i = 0; i < g.fat_sectors && r == PRE_OK; i++) {
            uint32_t s1 = g.fat_start + i, s2 = s1 + g.fat_sectors;
            if (!sec_read(s1, a, NULL) || (g.n_fats == 2 && !sec_read(s2, b, NULL))) { r = PRE_ERROR; break; }
            fat_verdict_t v = fat_mirror_compare(a, g.n_fats == 2 ? b : a, ss, i == 0, g.media);
            if (v == FAT_BOTH_BAD) r = PRE_BOTH_BAD;
            else if (v == FAT_COPY_2_TO_1 || v == FAT_COPY_1_TO_2) {
                bool to1 = v == FAT_COPY_2_TO_1;
                bool ok = sec_write(to1 ? s1 : s2, to1 ? b : a, NULL);
                s_repairs += ok;
                ESP_LOGE(TAG, "NAND: FAT %d sector %lu differs after a reset: rewritten from FAT %d: %s", to1 ? 1 : 2,
                         (unsigned long)i, to1 ? 2 : 1, ok ? "ok" : "FAILED");
                if (!ok) r = PRE_ERROR;
            }
        }
        if (r == PRE_OK) reclaim(&g, a);
        if (g.n_fats < 2) ESP_LOGW(TAG, "NAND: one FAT copy only: a torn FAT cannot be repaired");
    }
    if (s_repairs || s_scrubbed || s_reclaimed_kb) spi_nand_flash_sync(s_dev);
    free(a);
    free(b);
    free(c);
    return r;
}

/* Never written by the driver: every good block's first page carries no "page used" marker (nand_impl.c nand_prog writes
   one into every page it programs, and NAND pages are programmed in order from page 0, so a block in use has page 0 used). */
static bool raw_blank(void)
{
    uint32_t blk_sz = 0, pg_sz = 0;
    spi_nand_flash_get_block_size(s_dev, &blk_sz);
    spi_nand_flash_get_page_size(s_dev, &pg_sz);
    uint32_t ppb = pg_sz ? blk_sz / pg_sz : 64;
    for (uint32_t b = 0; b < s_blocks; b++) {
        bool bad = false, free_pg = true;
        if (nand_wrap_is_bad(s_dev, b, &bad) != ESP_OK) return false;
        if (bad) continue;
        if (nand_wrap_is_free(s_dev, b * ppb, &free_pg) != ESP_OK || !free_pg) {
            ESP_LOGI(TAG, "NAND: block %lu is in use: not blank", (unsigned long)b);
            return false;
        }
    }
    return true;
}

/* 2 FAT copies (the component's own format-on-mount passes n_fat 0, which FatFs turns into ONE FAT: ff.c f_mkfs),
   no partition table, FAT12/16 only (fat_reclaim walks those). */
static esp_err_t make_fat(void)
{
    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK) return ESP_ERR_NO_MEM;
    ff_diskio_register_nand(pdrv, s_dev);
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    void *work = ff_memalloc(FF_MAX_SS);
    const MKFS_PARM opt = { FM_FAT | FM_SFD, 2, 0, 0, AU_BYTES };
    FRESULT fr = work ? f_mkfs(drv, &opt, work, FF_MAX_SS) : FR_NOT_ENOUGH_CORE;
    free(work);
    esp_err_t serr = spi_nand_flash_sync(s_dev);
    ff_diskio_unregister(pdrv);
    ff_diskio_clear_pdrv_nand(s_dev);
    ESP_LOGW(TAG, "NAND: FAT made (2 copies, %d KB clusters): %s", AU_BYTES / 1024, fr == FR_OK && serr == ESP_OK ? "ok" : "FAILED");
    return fr == FR_OK && serr == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t do_mount(void)
{
    const esp_vfs_fat_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = AU_BYTES };
    return esp_vfs_fat_nand_mount(CONFIG_OCM_NAND_MOUNT_POINT, s_dev, &mc);
}

/* erase every good block (spi_nand_erase_chip also clears the Dhara map), make the FAT, mount; the NVS flag marks the
   window, so a reset inside it is redone at the next boot. Caller holds s_nlock, the volume is not mounted. */
static esp_err_t format_locked(void)
{
    s_state = NAND_FAILED;
    fmt_flag(1);
    esp_err_t err = spi_nand_erase_chip(s_dev);
    if (err == ESP_ERR_NOT_FINISHED) { ESP_LOGW(TAG, "NAND: a block failed to erase (worn out): the FTL skips it"); err = ESP_OK; }
    if (err == ESP_OK) err = make_fat();
    if (err == ESP_OK) err = do_mount();
    if (err == ESP_OK) { fmt_flag(0); s_state = NAND_MOUNTED; }
    return err;
}

bool nand_store_info(uint64_t *total, uint64_t *free_b)
{
    return s_state == NAND_MOUNTED && esp_vfs_fat_info(CONFIG_OCM_NAND_MOUNT_POINT, total, free_b) == ESP_OK;
}

/* the flash FAT's boot audit (sdlog.c storage_audit) for the NAND volume: space the FAT says is used vs what the files need */
static uint64_t tree_bytes(const char *dir, uint32_t clu, int *files, int depth)
{
    uint64_t sum = clu;
    DIR *d = depth < 6 ? opendir(dir) : NULL;
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        char p[160];
        struct stat st;
        snprintf(p, sizeof p, "%s/%.40s", dir, e->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) sum += tree_bytes(p, clu, files, depth + 1);
        else { sum += ((uint64_t)st.st_size + clu - 1) / clu * clu; (*files)++; }
    }
    if (d) closedir(d);
    return sum;
}

static void audit(void)
{
    BYTE pdrv = ff_diskio_get_pdrv_nand(s_dev);
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    FATFS *fs = NULL;
    DWORD free_cl = 0;
    if (pdrv == 0xFF || f_getfree(drv, &free_cl, &fs) != FR_OK || !fs) { ESP_LOGW(TAG, "NAND audit: f_getfree failed"); return; }
    uint32_t clu = (uint32_t)fs->csize * fs->ssize;
    uint64_t total = (uint64_t)(fs->n_fatent - 2) * clu, freeb = (uint64_t)free_cl * clu, used = total - freeb;
    int files = 0;
    uint64_t need = tree_bytes(CONFIG_OCM_NAND_MOUNT_POINT, clu, &files, 0);
    bool bad = used > need + need / 10 + 2 * clu;
    ESP_LOGI(TAG, "NAND audit: FAT%d, %d FAT cop%s, %lu B clusters: %llu KB used by the FAT, %llu KB needed by %d files, %llu of %llu KB free%s",
             fs->fs_type == FS_FAT12 ? 12 : fs->fs_type == FS_FAT16 ? 16 : 32, fs->n_fats, fs->n_fats == 1 ? "y" : "ies",
             (unsigned long)clu, (unsigned long long)(used / 1024), (unsigned long long)(need / 1024), files,
             (unsigned long long)(freeb / 1024), (unsigned long long)(total / 1024), bad ? "  <- LOST CLUSTERS" : "");
    if (bad) sys_event(EV_ERROR, "Drive store: %llu KB used, files %llu KB. Format",
                       (unsigned long long)(used / 1024), (unsigned long long)(need / 1024));
}

#if CONFIG_OCM_NAND_SELFTEST
#include "esp_timer.h"
#include <unistd.h>
static int cmp_i64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }

/* bench: sequential write + sync, read-back, and the logger's pattern (1 KB appends, fsync every 25) */
static void selftest(void)
{
    const char *path = CONFIG_OCM_NAND_MOUNT_POINT "/selftest.bin";
    uint8_t *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    FILE *f = fopen(path, "wb");
    int64_t t0 = esp_timer_get_time();
    bool ok = f != NULL;
    for (int i = 0; ok && i < 64; i++) { memset(buf, (uint8_t)i, 16384); ok = fwrite(buf, 1, 16384, f) == 16384; }
    if (f) { fflush(f); fsync(fileno(f)); fclose(f); }
    int64_t t1 = esp_timer_get_time();
    f = ok ? fopen(path, "rb") : NULL;
    for (int i = 0; f && ok && i < 64; i++) {
        ok = fread(buf, 1, 16384, f) == 16384;
        for (int k = 0; ok && k < 16384; k++) ok = buf[k] == (uint8_t)i;
    }
    if (f) fclose(f);
    int64_t t2 = esp_timer_get_time();
    unlink(path);
    static int64_t sync_us[8];
    int ns = 0;
    f = fopen(path, "ab");
    memset(buf, 'x', 1024);
    for (int i = 1; f && i <= 200; i++) {
        fwrite(buf, 1, 1024, f);
        if (i % 25 == 0) { int64_t a = esp_timer_get_time(); fflush(f); fsync(fileno(f)); sync_us[ns++] = esp_timer_get_time() - a; }
    }
    if (f) fclose(f);
    unlink(path);
    qsort(sync_us, ns, sizeof sync_us[0], cmp_i64);
    ESP_LOGW(TAG, "NAND selftest: write %lld KB/s, read %lld KB/s, verify %s, 25-row fsync p50 %lld ms max %lld ms",
             t1 > t0 ? 1024LL * 1000000 / (t1 - t0) : 0, t2 > t1 ? 1024LL * 1000000 / (t2 - t1) : 0, ok ? "PASS" : "FAIL",
             ns ? sync_us[ns / 2] / 1000 : -1, ns ? sync_us[ns - 1] / 1000 : -1);
    free(buf);
}
#endif

esp_err_t nand_store_init(void)
{
    if (!s_nlock) s_nlock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_nlock, portMAX_DELAY);
    esp_err_t err = bus_init();
    if (err == ESP_OK) {
        read_id();
        spi_nand_flash_config_t cfg = {
            .device_handle = s_spi, .gc_factor = 0,
            .io_mode = NAND_QIO ? SPI_NAND_IO_MODE_QIO : SPI_NAND_IO_MODE_SIO, .flags = SPI_DEVICE_HALFDUPLEX,
        };
        err = spi_nand_flash_init_device(&cfg, &s_dev);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SPI NAND on SPI3 (id %02X %02X): %s; drive logs stay on the flash FAT", s_mi, s_di, esp_err_to_name(err));
        s_state = NAND_ABSENT;
        xSemaphoreGive(s_nlock);
        return err;
    }
    uint32_t pages = 0, pg = 0;
    spi_nand_flash_get_block_num(s_dev, &s_blocks);
    spi_nand_flash_get_page_count(s_dev, &pages);
    spi_nand_flash_get_page_size(s_dev, &pg);
    nand_get_bad_block_stats(s_dev, &s_bad);
    snprintf(s_desc, sizeof s_desc, "SPI NAND %02X/%02X, %lu blocks, %lu bad, %s %d MHz", s_mi, s_di, (unsigned long)s_blocks,
             (unsigned long)s_bad, NAND_QIO ? "QIO" : "SIO", CONFIG_OCM_NAND_SPI_MHZ);
    ESP_LOGI(TAG, "%s; Dhara map %lu sectors of %lu B (%llu KB)", s_desc, (unsigned long)pages, (unsigned long)pg,
             (unsigned long long)pages * pg / 1024);

    if (fmt_was_interrupted()) {
        /* a format (first use, or Settings) was cut by a reset: the chip held no drive logs then, so it is redone */
        ESP_LOGW(TAG, "NAND: a format was interrupted: formatting again");
        err = format_locked();
        if (err != ESP_OK) s_state = NAND_FAILED;
    } else {
        pre_t pre = preflight();
        if (pre == PRE_BOTH_BAD) {
            s_state = NAND_UNREADABLE;
            ESP_LOGE(TAG, "NAND: BOTH FAT copies are damaged: not mounted, nothing written");
        } else if ((err = do_mount()) == ESP_FAIL) {
            if (pre == PRE_NO_FAT && raw_blank()) {
                ESP_LOGW(TAG, "NAND: blank chip (first use): formatting it");
                fmt_flag(1);
                err = make_fat();
                if (err == ESP_OK) err = do_mount();
                if (err == ESP_OK) { fmt_flag(0); sys_event(EV_INFO, "Drive store: the 128 MB NAND was blank and is formatted"); }
                else s_state = NAND_FAILED;
            } else {
                s_state = NAND_UNREADABLE;
                ESP_LOGE(TAG, "NAND: holds data but no FAT FatFs can mount: not formatted, nothing written");
            }
        } else if (err != ESP_OK) s_state = NAND_FAILED;
    }
    if (err == ESP_OK && s_state != NAND_UNREADABLE) {
        s_state = NAND_MOUNTED;
        if (s_repairs) sys_event(EV_WARN, "Drive store: %d FAT sector%.1s repaired from the copy", s_repairs,
                                 s_repairs == 1 ? "" : "s");
        if (s_scrubbed) sys_event(EV_ERROR, "Drive store: reset tore a folder, %d files lost", s_scrubbed);
        audit();
#if CONFIG_OCM_NAND_SELFTEST
        selftest();
#endif
    }
    if (s_state == NAND_UNREADABLE)
        sys_event(EV_ERROR, "Drive store unreadable, left as is. Format erases it");
    else if (s_state == NAND_FAILED) sys_event(EV_ERROR, "Drive store off: %s; flash used", esp_err_to_name(err));
    xSemaphoreGive(s_nlock);
    return s_state == NAND_MOUNTED ? ESP_OK : ESP_FAIL;
}

esp_err_t nand_store_format(char *why, size_t why_n)
{
    if (!s_dev) { snprintf(why, why_n, "no NAND"); return ESP_ERR_INVALID_STATE; }
    if (s_quiesced || xSemaphoreTake(s_nlock, pdMS_TO_TICKS(5000)) != pdTRUE) { snprintf(why, why_n, "NAND busy"); return ESP_ERR_TIMEOUT; }
    if (s_state == NAND_MOUNTED) esp_vfs_fat_nand_unmount(CONFIG_OCM_NAND_MOUNT_POINT, s_dev);
    esp_err_t err = format_locked();
    if (err == ESP_OK) { s_repairs = s_scrubbed = 0; s_reclaimed_kb = 0; }
    else snprintf(why, why_n, "NAND format failed: %s", esp_err_to_name(err));
    nand_get_bad_block_stats(s_dev, &s_bad);
    ESP_LOGW(TAG, "NAND formatted on request: %s (%lu bad blocks)", esp_err_to_name(err), (unsigned long)s_bad);
    xSemaphoreGive(s_nlock);
    return err;
}

void nand_store_quiesce(void)
{
    if (s_quiesced) return;
    s_quiesced = true;
    if (s_nlock && xSemaphoreTake(s_nlock, pdMS_TO_TICKS(20000)) != pdTRUE)
        ESP_LOGW(TAG, "quiesce: NAND mount or format still running after 20 s");
    if (s_state != NAND_MOUNTED || !s_dev) return;
    BYTE pdrv = ff_diskio_get_pdrv_nand(s_dev);
    if (pdrv != 0xFF && !ff_mutex_take(pdrv)) ESP_LOGW(TAG, "quiesce: the NAND file system stayed busy for %d ms", CONFIG_FATFS_TIMEOUT_MS);
    spi_nand_flash_sync(s_dev);                          /* Dhara checkpoint: no rollback past this point */
}

void nand_store_unmount(void)
{
    if (!s_nlock || xSemaphoreTake(s_nlock, pdMS_TO_TICKS(5000)) != pdTRUE) return;
    if (s_state == NAND_MOUNTED) {
        esp_vfs_fat_nand_unmount(CONFIG_OCM_NAND_MOUNT_POINT, s_dev);
        spi_nand_flash_sync(s_dev);
        s_state = NAND_ABSENT;
    }
    xSemaphoreGive(s_nlock);
}
#endif
