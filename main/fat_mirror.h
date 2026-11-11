#pragma once
/* FAT copy check for a FAT12/16 volume on NOR flash (no IDF dependency, host-tested in tests/host/test_fat_mirror.c).
 *
 * Why: the flash FAT's disk layer writes a sector as erase-then-program (ESP-IDF diskio_wl.c:50-55), so a reset between
 * the two leaves the sector erased (all 0xFF), or programmed only up to the reset. FatFs writes the first FAT, then the
 * same window to the second (ff.c sync_window, 1066-1071) and never reads the second FAT back. A torn first FAT is
 * therefore copied over the good second FAT by the next allocation, and the volume shows every cluster as used. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sector_size;     /* bytes per sector (BPB) */
    uint32_t fat_start;       /* first sector of FAT 1, from the start of the medium */
    uint32_t fat_sectors;     /* sectors per FAT */
    uint8_t  n_fats;
    uint8_t  media;           /* BPB media byte, also the low byte of FAT entry 0 */
    uint8_t  fat_bits;        /* 12 or 16 (0 = FAT32, which fat_reclaim does not handle) */
    uint32_t sec_per_clus;
    uint32_t root_start;      /* FAT12/16 fixed root directory */
    uint32_t root_sectors;
    uint32_t data_start;      /* sector of cluster 2 */
    uint32_t n_clusters;      /* data clusters: valid cluster numbers are 2 .. n_clusters + 1 */
} fat_geom_t;

/* Boot sector (VBR at sector 0, or an MBR whose first partition holds it) -> geometry. sec0 is sector 0 of the medium;
   read_sector reads another sector when the volume sits in a partition (may be NULL: then only a VBR at 0 is accepted). */
bool fat_geom_parse(const uint8_t *sec0, size_t n, bool (*read_sector)(uint32_t lba, uint8_t *buf, void *ctx), void *ctx,
                    uint8_t *scratch, fat_geom_t *g);

typedef enum {
    FAT_SAME = 0,             /* identical: nothing to do */
    FAT_COPY_2_TO_1,          /* FAT 1 is torn or newer than the committed FAT 2: FAT 2 is written over FAT 1 */
    FAT_COPY_1_TO_2,          /* FAT 2 was torn while FAT 1 was complete: FAT 1 is written over FAT 2 */
    FAT_BOTH_BAD,             /* neither copy is a valid first FAT sector: do not let FatFs write */
} fat_verdict_t;

/* One sector of each FAT copy (the same index i). first = sector 0 of the FAT (holds entries 0 and 1). */
fat_verdict_t fat_mirror_compare(const uint8_t *fat1, const uint8_t *fat2, size_t n, bool first, uint8_t media);

/* Orphan reclaim (a minimal fsck for FAT12/16). A file's clusters reach the FAT before its directory entry records its
 * first cluster (FatFs writes the entry at f_sync/f_close), so a reset while a new file is written leaves a chain that no
 * directory entry reaches; nothing ever frees it. This walks the directory tree, marks every cluster a directory or file
 * chain reaches, and clears allocated entries that nothing reaches.
 *
 * A directory sector torn by a reset (erased, or programmed only part way) holds entries whose name starts with 0xFF,
 * which no real name does. With write_sector those entries are rewritten as deleted entries (0xE5, so FatFs keeps reading
 * the entries after them) and *scrubbed counts them; their files are gone, and their chains are freed with the orphans.
 * Without write_sector such a sector stops the walk.
 *
 * fat: the whole first FAT (fat_sectors * sector_size bytes), changed in place. Returns the number of clusters freed, or
 * -1 when the tree is not clean enough to touch (then fat is unchanged): a read or write error, a torn directory sector
 * without write_sector, a chain that leaves the cluster range or runs into a free or bad cluster, two chains that share a
 * cluster, or a tree deeper than 16. */
int fat_reclaim(const fat_geom_t *g, uint8_t *fat, bool (*read_sector)(uint32_t lba, uint8_t *buf, void *ctx),
                bool (*write_sector)(uint32_t lba, const uint8_t *buf, void *ctx), void *ctx, uint8_t *secbuf, uint8_t *mark,
                int *scrubbed);
/* bytes the mark bitmap needs */
#define FAT_RECLAIM_MARK_BYTES(g) (((g)->n_clusters + 2 + 7) / 8)
