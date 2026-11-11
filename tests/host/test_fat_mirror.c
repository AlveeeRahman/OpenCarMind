/* fat_mirror.c: which FAT copy wins after a torn write (see main/fat_mirror.h). */
#include <stdio.h>
#include <string.h>
#include "fat_mirror.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SS 4096
static uint8_t f1[SS], f2[SS], img[3 * SS];

/* A FAT12 first sector: entries 0/1, then a chain 2->3->EOC, cluster 4 EOC, the rest free, 0xFF after the last entry. */
static void fat12(uint8_t *p, int extra)
{
    memset(p, 0, SS);
    memset(p + 1418, 0xFF, SS - 1418);
    p[0] = 0xF8; p[1] = 0xFF; p[2] = 0xFF;
    p[3] = 0x03; p[4] = 0xF0; p[5] = 0xFF;   /* entry 2 = 0x003, entry 3 = 0xFFF */
    p[6] = 0xFF; p[7] = 0x0F;                 /* entry 4 = 0xFFF, entry 5 = 0 */
    if (extra) { p[7] = 0xFF; p[8] = 0xFF; }  /* entries 5, 6 = EOC: two more single-cluster files */
}

static size_t s_rd_ss = SS;
static bool rd(uint32_t lba, uint8_t *buf, void *ctx)
{
    (void)ctx;
    if ((lba + 1) * s_rd_ss > sizeof img) return false;
    memcpy(buf, img + lba * s_rd_ss, s_rd_ss);
    return true;
}

static bool wr(uint32_t lba, const uint8_t *buf, void *ctx)
{
    (void)ctx;
    if ((lba + 1) * s_rd_ss > sizeof img) return false;
    memcpy(img + lba * s_rd_ss, buf, s_rd_ss);
    return true;
}

int main(void)
{
    /* identical copies */
    fat12(f1, 0); fat12(f2, 0);
    CHECK(fat_mirror_compare(f1, f2, SS, true, 0xF8) == FAT_SAME);

    /* FAT 1 erased by a reset between erase and program (the incident's first step) */
    memset(f1, 0xFF, SS);
    CHECK(fat_mirror_compare(f1, f2, SS, true, 0xF8) == FAT_COPY_2_TO_1);

    /* FAT 2 erased while FAT 1 was already complete and newer */
    fat12(f1, 1); memset(f2, 0xFF, SS);
    CHECK(fat_mirror_compare(f1, f2, SS, true, 0xF8) == FAT_COPY_1_TO_2);

    /* FAT 2 programmed only up to byte 6, FAT 1 complete */
    fat12(f1, 1); memcpy(f2, f1, 6); memset(f2 + 6, 0xFF, SS - 6);
    CHECK(fat_mirror_compare(f1, f2, SS, true, 0xF8) == FAT_COPY_1_TO_2);

    /* FAT 1 programmed only up to byte 6 (new data), FAT 2 old: FAT 2 wins */
    fat12(f2, 0); fat12(f1, 1); memset(f1 + 6, 0xFF, SS - 6);
    CHECK(fat_mirror_compare(f1, f2, SS, true, 0xF8) == FAT_COPY_2_TO_1);

    /* both complete but different (reset between the FAT 1 and FAT 2 writes): FAT 2, the copy the directory agrees with */
    fat12(f1, 1); fat12(f2, 0);
    CHECK(fat_mirror_compare(f1, f2, SS, true, 0xF8) == FAT_COPY_2_TO_1);

    /* the v11u dump: FAT 1 == FAT 2, all 0xFF except bytes 60/61 = 51 F0 (FatFs mirrored the erased FAT 1) */
    memset(f1, 0xFF, SS); f1[60] = 0x51; f1[61] = 0xF0; memcpy(f2, f1, SS);
    CHECK(fat_mirror_compare(f1, f2, SS, true, 0xF8) == FAT_BOTH_BAD);
    /* both erased */
    memset(f1, 0xFF, SS); memset(f2, 0xFF, SS);
    CHECK(fat_mirror_compare(f1, f2, SS, true, 0xF8) == FAT_BOTH_BAD);
    /* one FAT (old USB-disk format) and a valid sector: compared with itself */
    fat12(f1, 0);
    CHECK(fat_mirror_compare(f1, f1, SS, true, 0xF8) == FAT_SAME);
    /* a later FAT sector (no header) erased in FAT 1 */
    memset(f1, 0x11, SS); memset(f2, 0x11, SS); memset(f1, 0xFF, SS);
    CHECK(fat_mirror_compare(f1, f2, SS, false, 0xF8) == FAT_COPY_2_TO_1);

    /* geometry: the IDF mkfs boot sector of the board (4096 B sectors, 1 reserved, 2 FATs of 1 sector, media F8) */
    fat_geom_t g;
    memset(img, 0, sizeof img);
    uint8_t *b = img;
    b[0] = 0xEB; b[11] = 0x00; b[12] = 0x10; b[13] = 1; b[14] = 1; b[16] = 2; b[21] = 0xF8; b[22] = 1; b[510] = 0x55; b[511] = 0xAA;
    CHECK(fat_geom_parse(b, SS, rd, NULL, f1, &g) && g.sector_size == 4096 && g.fat_start == 1 && g.fat_sectors == 1 &&
          g.n_fats == 2 && g.media == 0xF8);
    /* the same volume behind an MBR at LBA 2 */
    memcpy(img + 2 * SS, b, SS);
    memset(b, 0, SS); b[446 + 4] = 0x01; b[446 + 8] = 2; b[510] = 0x55; b[511] = 0xAA;
    CHECK(fat_geom_parse(b, SS, rd, NULL, f1, &g) && g.fat_start == 3 && g.n_fats == 2);
    /* erased sector 0 */
    memset(b, 0xFF, SS);
    CHECK(!fat_geom_parse(b, SS, rd, NULL, f1, &g));

    s_rd_ss = 512;
    /* ---- orphan reclaim on a FAT12 image: 512 B sectors, 1 reserved, 2 FATs of 1 sector, 16 root entries, 20 clusters ---- */
    {
        static uint8_t v[24 * 512];
        memset(v, 0, sizeof v);
        uint8_t *bs = v;
        bs[0] = 0xEB; bs[11] = 0x00; bs[12] = 0x02; bs[13] = 1; bs[14] = 1; bs[16] = 2; bs[17] = 16; bs[19] = 24; bs[21] = 0xF8;
        bs[22] = 1; bs[510] = 0x55; bs[511] = 0xAA;
        fat_geom_t vg;
        CHECK(fat_geom_parse(bs, 512, NULL, NULL, NULL, &vg) && vg.fat_bits == 12 && vg.root_start == 3 && vg.data_start == 4 &&
              vg.n_clusters == 20 && vg.root_sectors == 1);
        uint8_t fat[512], sec[512], mark[8];
        /* entries: 2->3->EOC (file A), 4 EOC (dir D), 5 EOC (file B in D), 6->7->EOC orphan, 8 EOC (only a deleted entry) */
        uint16_t ent[12] = {0xFF8, 0xFFF, 3, 0xFFF, 0xFFF, 0xFFF, 7, 0xFFF, 0xFFF, 0, 0, 0};
        memset(fat, 0, sizeof fat);
        for (int c = 0; c < 12; c++) {
            uint8_t *q = fat + c * 3 / 2;
            if (c & 1) { q[0] = (uint8_t)((q[0] & 0x0F) | (ent[c] << 4)); q[1] = (uint8_t)(ent[c] >> 4); }
            else { q[0] = (uint8_t)ent[c]; q[1] = (uint8_t)((q[1] & 0xF0) | (ent[c] >> 8)); }
        }
        uint8_t *root = v + 3 * 512, *dd = v + (4 + 4 - 2) * 512;
        memcpy(root, "A       TXT", 11); root[11] = 0x20; root[26] = 2;
        memset(root + 32, 0x41, 11); root[32 + 11] = 0x0F;                       /* an LFN entry */
        memcpy(root + 64, "D          ", 11); root[64 + 11] = 0x10; root[64 + 26] = 4;
        memcpy(root + 96, "\xE5" "ELETED BIN", 11); root[96 + 11] = 0x20; root[96 + 26] = 8;
        memcpy(dd, ".          ", 11); dd[11] = 0x10; dd[26] = 4;
        memcpy(dd + 32, "..         ", 11); dd[32 + 11] = 0x10;
        memcpy(dd + 64, "B       BIN", 11); dd[64 + 11] = 0x20; dd[64 + 26] = 5;
        memcpy(img, v, sizeof v);
        static uint8_t work[512];
        int sc = -1;
        memcpy(work, fat, 512);
        CHECK(fat_reclaim(&vg, work, rd, wr, NULL, sec, mark, &sc) == 3 && sc == 0);   /* 6, 7 and 8 freed */
        CHECK(work[0] == 0xF8 && work[9] == 0 && (work[10] >> 4) == 0 && work[12] == 0 && (work[13] & 0x0F) == 0);
        CHECK(memcmp(work, fat, 9) == 0);                                        /* A, D, B untouched */
        CHECK(fat_reclaim(&vg, work, rd, wr, NULL, sec, mark, &sc) == 0);       /* idempotent */
        /* D's directory sector erased (torn): read-only walk refuses and changes nothing */
        memset(img + (4 + 4 - 2) * 512, 0xFF, 512);
        memcpy(work, fat, 512);
        CHECK(fat_reclaim(&vg, work, rd, NULL, NULL, sec, mark, &sc) == -1 && memcmp(work, fat, 512) == 0);
        /* with a writer: the 16 entries become deleted ones, B's chain (5) is freed with 6, 7, 8 */
        CHECK(fat_reclaim(&vg, work, rd, wr, NULL, sec, mark, &sc) == 4 && sc == 16);
        CHECK(img[(4 + 4 - 2) * 512] == 0xE5 && img[(4 + 4 - 2) * 512 + 11] == 0 && img[(4 + 4 - 2) * 512 + 32 * 15] == 0xE5);
        /* D's sector programmed only up to entry 2 (".", ".." kept, B lost): 14 entries scrubbed */
        memcpy(img, v, sizeof v); memset(img + (4 + 4 - 2) * 512 + 64, 0xFF, 512 - 64);
        memcpy(work, fat, 512);
        CHECK(fat_reclaim(&vg, work, rd, wr, NULL, sec, mark, &sc) == 4 && sc == 14 && img[(4 + 4 - 2) * 512] == '.');
        /* a chain that runs into a free cluster: hands off */
        memcpy(img, v, sizeof v); memcpy(work, fat, 512);
        work[4] &= 0x0F; work[5] = 0;                                             /* entry 3 = 0 */
        uint8_t keep[512]; memcpy(keep, work, 512);
        CHECK(fat_reclaim(&vg, work, rd, wr, NULL, sec, mark, &sc) == -1 && memcmp(work, keep, 512) == 0);
    }

    printf("fat_mirror: %d checks, %d failures\n", runs, fails);
    return fails != 0;
}
