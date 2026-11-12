/* F1: the flash FAT's repair rules (main/fat_mirror.c) on the NAND's volume shape, under Dhara's reset behaviour.
 *
 * The NAND FAT (nand_store.c make_fat) is FAT16 with 2048-byte sectors and 2 FAT copies. Dhara writes a sector atomically
 * and a reset rolls the map back to the last sync point, so after a reset the volume is the image after some PREFIX of the
 * sector writes FatFs made (never a half-erased sector, the NOR case). This test records the sector writes of an append
 * (data, FAT 1, FAT 2, directory: FatFs's f_write + f_sync order) and of a delete (directory, FAT 1, FAT 2: f_unlink),
 * applies every prefix, runs nand_store.c's preflight steps (compare each FAT sector pair, then fat_reclaim) and checks the
 * volume is consistent: both FATs equal, no cluster allocated that no file reaches, every file's chain covers its size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat_mirror.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SS       2048
#define TOT      4400                   /* sectors: 4385 one-sector clusters = FAT16 (>= 4085) */
#define RSV      1
#define FATSZ    5                      /* (4385 + 2) * 2 B = 8774 B -> 5 sectors */
#define ROOTENT  512                    /* 512 * 32 B = 8 sectors */
#define MEDIA    0xF8

static uint8_t vol[TOT][SS];
static fat_geom_t g;

static bool rd(uint32_t lba, uint8_t *buf, void *ctx) { (void)ctx; if (lba >= TOT) return false; memcpy(buf, vol[lba], SS); return true; }
static bool wr(uint32_t lba, const uint8_t *buf, void *ctx) { (void)ctx; if (lba >= TOT) return false; memcpy(vol[lba], buf, SS); return true; }

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void mkfs(void)
{
    memset(vol, 0, sizeof vol);
    uint8_t *b = vol[0];
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    le16(b + 11, SS); b[13] = 1; le16(b + 14, RSV); b[16] = 2; le16(b + 17, ROOTENT); le16(b + 19, TOT); b[21] = MEDIA;
    le16(b + 22, FATSZ);
    b[510] = 0x55; b[511] = 0xAA;
    for (int c = 0; c < 2; c++) { uint8_t *f = vol[RSV + c * FATSZ]; f[0] = MEDIA; f[1] = 0xFF; f[2] = 0xFF; f[3] = 0xFF; }
}

/* a pending write list: what FatFs sends to the disk, in order */
typedef struct { uint32_t lba; uint8_t data[SS]; } w_t;
static w_t wl[16];
static int nw;
static void queue(uint32_t lba, const uint8_t *data) { wl[nw].lba = lba; memcpy(wl[nw].data, data, SS); nw++; }

static uint16_t fat_get(const uint8_t *fat1, uint32_t c) { return rd16(fat1 + 2 * c); }

/* the whole first FAT as one buffer */
static void read_fat(uint8_t *fat) { for (int i = 0; i < FATSZ; i++) memcpy(fat + i * SS, vol[RSV + i], SS); }

static uint8_t *root(void) { return vol[RSV + 2 * FATSZ]; }

/* f_write of one cluster + f_sync on root entry 0 ("S000001 CSV"): data, FAT 1, FAT 2, directory */
static void plan_append(uint32_t newc, const char *payload)
{
    nw = 0;
    static uint8_t fat[FATSZ * SS], sec[SS];
    read_fat(fat);
    uint8_t e[32];
    memcpy(e, root(), 32);
    uint16_t first = rd16(e + 26);
    uint32_t size = (uint32_t)e[28] | (uint32_t)e[29] << 8 | (uint32_t)e[30] << 16 | (uint32_t)e[31] << 24;
    memset(sec, 0, SS);
    snprintf((char *)sec, SS, "%s", payload);
    queue(g.data_start + newc - 2, sec);
    le16(fat + 2 * newc, 0xFFFF);
    if (first) { uint16_t c = first; while (fat_get(fat, c) < 0xFFF8) c = fat_get(fat, c); le16(fat + 2 * c, (uint16_t)newc); }
    uint32_t fs = 2 * newc / SS;                           /* the FAT sector holding the new entry (and, here, the link) */
    queue(RSV + fs, fat + fs * SS);
    queue(RSV + FATSZ + fs, fat + fs * SS);
    if (!first) le16(e + 26, (uint16_t)newc);
    le32(e + 28, size + SS);
    memcpy(sec, root(), SS);
    memcpy(sec, e, 32);
    queue(g.root_start, sec);
}

/* f_unlink of entry 0: directory (entry deleted), then the chain freed in FAT 1, FAT 2 */
static void plan_delete(void)
{
    nw = 0;
    static uint8_t fat[FATSZ * SS], sec[SS];
    read_fat(fat);
    memcpy(sec, root(), SS);
    uint16_t c = rd16(sec + 26);
    sec[0] = 0xE5;
    queue(g.root_start, sec);
    while (c >= 2 && c < 0xFFF8) { uint16_t n = fat_get(fat, c); le16(fat + 2 * c, 0); c = n; }
    queue(RSV, fat);                                       /* every cluster of this test lives in FAT sector 0 */
    queue(RSV + FATSZ, fat);
}

/* nand_store.c preflight: FAT pair compare per sector, then orphan reclaim written to both copies */
static int recover(void)
{
    uint8_t *a = vol[0];
    CHECK(fat_geom_parse(a, SS, rd, NULL, NULL, &g));
    for (uint32_t i = 0; i < g.fat_sectors; i++) {
        fat_verdict_t v = fat_mirror_compare(vol[g.fat_start + i], vol[g.fat_start + g.fat_sectors + i], SS, i == 0, g.media);
        CHECK(v != FAT_BOTH_BAD);
        if (v == FAT_COPY_2_TO_1) memcpy(vol[g.fat_start + i], vol[g.fat_start + g.fat_sectors + i], SS);
        if (v == FAT_COPY_1_TO_2) memcpy(vol[g.fat_start + g.fat_sectors + i], vol[g.fat_start + i], SS);
    }
    static uint8_t fat[FATSZ * SS], sec[SS], mark[FATSZ * SS];
    read_fat(fat);
    int scrubbed = 0, freed = fat_reclaim(&g, fat, rd, wr, NULL, sec, mark, &scrubbed);
    CHECK(freed >= 0);
    CHECK(scrubbed == 0);                                  /* Dhara never leaves a half-written directory sector */
    for (int c = 0; freed > 0 && c < 2; c++) for (int i = 0; i < FATSZ; i++) memcpy(vol[RSV + c * FATSZ + i], fat + i * SS, SS);
    return freed;
}

static void check_consistent(void)
{
    CHECK(!memcmp(vol[RSV], vol[RSV + FATSZ], (size_t)FATSZ * SS));           /* the two FATs agree */
    static uint8_t fat[FATSZ * SS], sec[SS], mark[FATSZ * SS];
    read_fat(fat);
    int scrubbed = 0;
    CHECK(fat_reclaim(&g, fat, rd, NULL, NULL, sec, mark, &scrubbed) == 0);   /* nothing left to reclaim */
    const uint8_t *e = root();
    if (e[0] != 0 && e[0] != 0xE5) {                       /* the file: its chain covers its size */
        uint32_t size = (uint32_t)e[28] | (uint32_t)e[29] << 8 | (uint32_t)e[30] << 16 | (uint32_t)e[31] << 24;
        uint32_t n = 0;
        for (uint16_t c = rd16(e + 26); c >= 2 && c < 0xFFF8 && n < 100; c = fat_get(fat, c)) n++;
        CHECK(n * SS >= size);
    }
}

int main(void)
{
    mkfs();
    CHECK(fat_geom_parse(vol[0], SS, rd, NULL, NULL, &g));
    CHECK(g.sector_size == 2048 && g.fat_bits == 16 && g.n_fats == 2 && g.fat_sectors == FATSZ && g.n_clusters >= 4085);
    memcpy(root(), "S000001 CSV", 11);
    root()[11] = 0x20;
    static uint8_t base[TOT][SS];
    int prefixes = 0, repaired = 0, reclaimed = 0;
    /* three appends then a delete; at each step every prefix of its writes is a possible state after a reset */
    for (int step = 0; step < 4; step++) {
        memcpy(base, vol, sizeof vol);
        if (step < 3) { char p[32]; snprintf(p, sizeof p, "rows of append %d", step); plan_append(2 + (uint32_t)step * 3, p); }
        else plan_delete();
        for (int k = 0; k <= nw; k++) {
            memcpy(vol, base, sizeof vol);
            for (int i = 0; i < k; i++) memcpy(vol[wl[i].lba], wl[i].data, SS);
            int differ = memcmp(vol[RSV], vol[RSV + FATSZ], (size_t)FATSZ * SS) != 0;
            int freed = recover();
            repaired += differ;
            reclaimed += freed > 0;
            check_consistent();
            prefixes++;
        }
        memcpy(vol, base, sizeof vol);                     /* go on from the complete step */
        for (int i = 0; i < nw; i++) memcpy(vol[wl[i].lba], wl[i].data, SS);
        check_consistent();
    }
    CHECK(repaired >= 4);                                  /* the FAT 1 written, FAT 2 not cases were met and repaired */
    CHECK(reclaimed >= 2);                                 /* the orphan cases (first append, delete) were met */
    /* the case fat_mirror cannot decide must still stop the mount: both first FAT sectors without a header */
    mkfs();
    vol[RSV][0] = 0; vol[RSV + FATSZ][0] = 0;
    CHECK(fat_mirror_compare(vol[RSV], vol[RSV + FATSZ], SS, true, MEDIA) == FAT_BOTH_BAD);
    printf("nand_fat: %d checks over %d reset points (%d FAT repairs, %d reclaims), %d failed\n", runs, prefixes, repaired,
           reclaimed, fails);
    return fails != 0;
}
