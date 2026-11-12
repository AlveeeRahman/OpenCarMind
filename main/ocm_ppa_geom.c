/* PPA and 2D-DMA helpers: rectangle, alignment and fallback rules (see ocm_ppa_geom.h) */
#include "ocm_ppa_geom.h"

ocm_slide_split_t ocm_slide_split(int32_t scroll_x, int ntiles, int32_t width)
{
    ocm_slide_split_t s = {0, 0, width, 0};
    if (ntiles < 1 || width <= 0) { s.w0 = 0; return s; }
    int32_t maxx = (int32_t)(ntiles - 1) * width;
    if (scroll_x < 0) scroll_x = 0;
    if (scroll_x > maxx) scroll_x = maxx;
    s.a = (int)(scroll_x / width);
    s.off = scroll_x % width;
    s.w0 = width - s.off;
    s.w1 = s.off;
    return s;
}

static uintptr_t align_down(uintptr_t v, uint32_t a) { return a ? v & ~(uintptr_t)(a - 1) : v; }
static uintptr_t align_up(uintptr_t v, uint32_t a) { return a ? (v + a - 1) & ~(uintptr_t)(a - 1) : v; }

bool ocm_blk_rebase(uintptr_t block, uint32_t stride_bytes, uint32_t bpp, uint32_t w, uint32_t h, uint32_t align, ocm_blk_t *out)
{
    if (!out || bpp == 0 || w == 0 || h == 0 || stride_bytes == 0) return false;
    if (align & (align - 1)) return false;
    if (stride_bytes % bpp) return false;
    uint32_t pic_w = stride_bytes / bpp;
    if (w > pic_w) return false;
    uintptr_t base = align_down(block, align);
    uintptr_t delta = block - base;
    uint32_t off_y = (uint32_t)(delta / stride_bytes), rem = (uint32_t)(delta % stride_bytes);
    if (rem % bpp) return false;
    uint32_t off_x = rem / bpp;
    if (off_x + w > pic_w) return false;
    uint32_t pic_h = off_y + h;
    if (pic_w > OCM_DMA2D_FIELD_MAX || pic_h > OCM_DMA2D_FIELD_MAX) return false;
    uint64_t size = (uint64_t)pic_w * pic_h * bpp;
    size = align_up((uintptr_t)size, align);
    if (size > UINT32_MAX) return false;
    out->base = base;
    out->pic_w = pic_w;
    out->pic_h = pic_h;
    out->off_x = off_x;
    out->off_y = off_y;
    out->buf_size = (uint32_t)size;
    out->win_lo = align_down(block, align);
    out->win_hi = align_up(block + (uintptr_t)(h - 1) * stride_bytes + (uintptr_t)w * bpp, align);
    return true;
}

bool ocm_region_has(const ocm_region_t *r, int n, uintptr_t lo, uintptr_t hi)
{
    if (!r || hi <= lo) return false;
    for (int i = 0; i < n; i++)
        if (r[i].lo <= lo && hi <= r[i].hi) return true;
    return false;
}

bool ocm_enc_row_ok(uint32_t pic_w, uint32_t block_w, uint32_t off_x, uint32_t bpp)
{
    const uint32_t m = OCM_MEMSPI_ENC_ALIGN - 1;
    return ((pic_w * bpp) & m) == 0 && ((block_w * bpp) & m) == 0 && ((off_x * bpp) & m) == 0;
}

void ocm_hw_note(ocm_hw_health_t *h, ocm_hw_res_t r)
{
    if (!h) return;
    switch (r) {
    case OCM_HW_OK: h->ok++; h->consec = 0; break;
    case OCM_HW_REFUSED: h->refused++; break;
    case OCM_HW_FAILED:
        h->failed++;
        if (++h->consec >= OCM_HW_MAX_FAIL) h->off = true;
        break;
    }
}

uint16_t ocm_rgb565_from_888(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int iabs(int v) { return v < 0 ? -v : v; }

bool ocm_rgb565_close(uint16_t a, uint16_t b, int tol)
{
    return iabs((a >> 11) - (b >> 11)) <= tol && iabs(((a >> 5) & 0x3F) - ((b >> 5) & 0x3F)) <= tol && iabs((a & 0x1F) - (b & 0x1F)) <= tol;
}
