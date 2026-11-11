#include "fat_mirror.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The same checks FatFs makes on a boot sector (ff.c check_fs/mount_volume): 0x55AA at 510, a jump instruction, a
   power-of-two sector size, a non-zero FAT size and one or two FATs. */
static bool vbr(const uint8_t *b, size_t n, uint32_t base, fat_geom_t *g)
{
    if (n < 512 || le16(b + 510) != 0xAA55 || !(b[0] == 0xEB || b[0] == 0xE9 || b[0] == 0xE8)) return false;
    uint32_t ss = le16(b + 11), rsv = le16(b + 14), fsz = le16(b + 22);
    if (!fsz) fsz = le32(b + 36);
    if (ss < 512 || ss > 4096 || (ss & (ss - 1)) || ss > n || !rsv || !fsz || b[16] < 1 || b[16] > 2) return false;
    uint32_t spc = b[13], rootent = le16(b + 17), tot = le16(b + 19);
    if (!tot) tot = le32(b + 32);
    if (!spc || (spc & (spc - 1))) return false;
    g->sector_size = ss;
    g->fat_start = base + rsv;
    g->fat_sectors = fsz;
    g->n_fats = b[16];
    g->media = b[21];
    g->sec_per_clus = spc;
    g->root_start = g->fat_start + g->n_fats * fsz;
    g->root_sectors = (rootent * 32 + ss - 1) / ss;
    g->data_start = g->root_start + g->root_sectors;
    uint32_t data = base + tot > g->data_start ? base + tot - g->data_start : 0;
    g->n_clusters = data / spc;
    /* the FAT type follows from the cluster count alone (FatFs mount_volume, Microsoft FAT spec) */
    g->fat_bits = !rootent ? 0 : g->n_clusters < 4085 ? 12 : g->n_clusters < 65525 ? 16 : 0;
    return true;
}

bool fat_geom_parse(const uint8_t *sec0, size_t n, bool (*read_sector)(uint32_t lba, uint8_t *buf, void *ctx), void *ctx,
                    uint8_t *scratch, fat_geom_t *g)
{
    if (vbr(sec0, n, 0, g)) return true;
    if (n < 512 || le16(sec0 + 510) != 0xAA55 || !sec0[446 + 4] || !read_sector || !scratch) return false;
    uint32_t lba = le32(sec0 + 446 + 8);                 /* first MBR partition (an old USB-disk format) */
    return lba && read_sector(lba, scratch, ctx) && vbr(scratch, n, lba, g);
}

static bool all_ff(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i] != 0xFF) return false;
    return true;
}

/* Entry 0 holds the media byte with every other bit set, entry 1 is end-of-chain: the first two bytes are media, 0xFF
   for FAT12, FAT16 and FAT32 alike. An erased or half-programmed first sector fails this. */
static bool header_ok(const uint8_t *p, uint8_t media) { return p[0] == media && p[1] == 0xFF; }

/* Torn: erased, or (first sector) no valid header, or erased from the first byte where it differs from the other copy
   to the end (a program cut short by the reset leaves the rest 0xFF). */
static bool torn(const uint8_t *x, const uint8_t *y, size_t n, bool first, uint8_t media)
{
    if (all_ff(x, n) || (first && !header_ok(x, media))) return true;
    size_t k = 0;
    while (k < n && x[k] == y[k]) k++;
    return k < n && all_ff(x + k, n - k);
}

static size_t programmed(const uint8_t *p, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += p[i] != 0xFF;
    return c;
}

fat_verdict_t fat_mirror_compare(const uint8_t *fat1, const uint8_t *fat2, size_t n, bool first, uint8_t media)
{
    if (first && !header_ok(fat1, media) && !header_ok(fat2, media)) return FAT_BOTH_BAD;
    if (!memcmp(fat1, fat2, n)) return FAT_SAME;
    bool t1 = torn(fat1, fat2, n, first, media), t2 = torn(fat2, fat1, n, first, media);
    if (t2 && !t1) return FAT_COPY_1_TO_2;
    if (t1 && t2) return programmed(fat1, n) > programmed(fat2, n) ? FAT_COPY_1_TO_2 : FAT_COPY_2_TO_1;
    /* FAT 1 torn, or both complete but different (reset between the two writes): FatFs writes FAT 2 only after FAT 1 was
       written without error, and the directory entry only after both, so FAT 2 is the copy the directory agrees with. */
    return FAT_COPY_2_TO_1;
}

/* ---------- orphan reclaim ---------- */
static uint32_t fat_get(const fat_geom_t *g, const uint8_t *fat, uint32_t c)
{
    if (g->fat_bits == 16) return le16(fat + 2 * c);
    uint32_t v = le16(fat + c * 3 / 2);
    return c & 1 ? v >> 4 : v & 0xFFF;
}

static void fat_put(const fat_geom_t *g, uint8_t *fat, uint32_t c, uint32_t v)
{
    if (g->fat_bits == 16) { fat[2 * c] = (uint8_t)v; fat[2 * c + 1] = (uint8_t)(v >> 8); return; }
    uint8_t *p = fat + c * 3 / 2;
    if (c & 1) { p[0] = (uint8_t)((p[0] & 0x0F) | (v << 4)); p[1] = (uint8_t)(v >> 4); }
    else { p[0] = (uint8_t)v; p[1] = (uint8_t)((p[1] & 0xF0) | ((v >> 8) & 0x0F)); }
}

typedef struct {
    const fat_geom_t *g;
    const uint8_t *fat;
    bool (*rd)(uint32_t, uint8_t *, void *);
    void *ctx;
    uint8_t *sec, *mark;
    uint32_t eoc_min, bad;
    bool (*wr)(uint32_t, const uint8_t *, void *);
    int scrubbed;
} rc_t;

static bool marked(const rc_t *r, uint32_t c) { return r->mark[c / 8] & (1u << (c % 8)); }
static void set_mark(rc_t *r, uint32_t c) { r->mark[c / 8] |= (uint8_t)(1u << (c % 8)); }
static bool in_range(const rc_t *r, uint32_t c) { return c >= 2 && c < r->g->n_clusters + 2; }

/* Marks a chain; false if it is not clean (out of range, into a free or bad cluster, or into a marked one). */
static bool mark_chain(rc_t *r, uint32_t c)
{
    for (uint32_t steps = 0; ; steps++) {
        if (!in_range(r, c) || marked(r, c) || steps > r->g->n_clusters) return false;
        set_mark(r, c);
        uint32_t n = fat_get(r->g, r->fat, c);
        if (n >= r->eoc_min) return true;
        if (n == 0 || n == r->bad) return false;
        c = n;
    }
}

static bool walk(rc_t *r, uint32_t first_cluster, int depth);

/* One directory sector: false on anything not clean; *end when the end-of-directory mark was found. */
static bool dir_sector(rc_t *r, uint32_t lba, int depth, bool *end)
{
    uint32_t ss = r->g->sector_size;
    if (!r->rd(lba, r->sec, r->ctx)) return false;
    /* No name starts with 0xFF: such entries are what an erase or a program cut short by a reset left. They are turned
       into deleted entries (not end marks, so FatFs still reads the entries after them), or the walk gives up. */
    int torn = 0;
    for (uint32_t off = 0; off < ss && r->sec[off] != 0; off += 32)        /* up to the end mark, as FatFs reads */
        if (r->sec[off] == 0xFF) { memset(r->sec + off, 0, 32); r->sec[off] = 0xE5; torn++; }
    if (torn) {
        if (!r->wr || !r->wr(lba, r->sec, r->ctx)) return false;
        r->scrubbed += torn;
    }
    for (uint32_t off = 0; off < ss; off += 32) {
        const uint8_t *e = r->sec + off;
        if (e[0] == 0) { *end = true; return true; }
        uint8_t attr = e[11];
        if (e[0] == 0xE5 || e[0] == '.' || (attr & 0x3F) == 0x0F || (attr & 0x08)) continue;   /* deleted, dot, LFN, label */
        uint32_t c = le16(e + 26);
        if (!c) continue;                                    /* empty file */
        if (attr & 0x10) {
            if (!walk(r, c, depth + 1) || !r->rd(lba, r->sec, r->ctx)) return false;   /* the walk reused the buffer */
        } else if (!mark_chain(r, c)) return false;
    }
    return true;
}

/* A directory: cluster 0 = the FAT12/16 root. Its own clusters are marked as it is read. */
static bool walk(rc_t *r, uint32_t c, int depth)
{
    if (depth > 16) return false;
    bool end = false;
    if (!c) {
        for (uint32_t i = 0; i < r->g->root_sectors && !end; i++)
            if (!dir_sector(r, r->g->root_start + i, depth, &end)) return false;
        return true;
    }
    for (uint32_t steps = 0; !end; steps++) {
        if (!in_range(r, c) || marked(r, c) || steps > r->g->n_clusters) return false;
        set_mark(r, c);
        uint32_t lba = r->g->data_start + (c - 2) * r->g->sec_per_clus;
        for (uint32_t i = 0; i < r->g->sec_per_clus && !end; i++)
            if (!dir_sector(r, lba + i, depth, &end)) return false;
        uint32_t n = fat_get(r->g, r->fat, c);
        if (n >= r->eoc_min) return true;
        if (n == 0 || n == r->bad) return false;
        c = n;
    }
    return true;
}

int fat_reclaim(const fat_geom_t *g, uint8_t *fat, bool (*read_sector)(uint32_t lba, uint8_t *buf, void *ctx),
                bool (*write_sector)(uint32_t lba, const uint8_t *buf, void *ctx), void *ctx, uint8_t *secbuf, uint8_t *mark,
                int *scrubbed)
{
    if (g->fat_bits != 12 && g->fat_bits != 16) return -1;
    if ((uint64_t)(g->n_clusters + 2) * g->fat_bits / 8 > (uint64_t)g->fat_sectors * g->sector_size) return -1;
    rc_t r = {g, fat, read_sector, ctx, secbuf, mark, g->fat_bits == 12 ? 0xFF8u : 0xFFF8u, g->fat_bits == 12 ? 0xFF7u : 0xFFF7u,
              write_sector, 0};
    memset(mark, 0, FAT_RECLAIM_MARK_BYTES(g));
    bool ok = walk(&r, 0, 0);
    if (scrubbed) *scrubbed = r.scrubbed;
    if (!ok) return -1;
    int freed = 0;
    for (uint32_t c = 2; c < g->n_clusters + 2; c++) {
        uint32_t v = fat_get(g, fat, c);
        if (v != 0 && v != r.bad && !marked(&r, c)) { fat_put(g, fat, c, 0); freed++; }
    }
    return freed;
}
