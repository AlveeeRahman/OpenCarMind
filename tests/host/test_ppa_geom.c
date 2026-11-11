/* Host tests for main/ocm_ppa_geom.c: the slide split, the aligned-base rebase, the PSRAM region rule, the flash-encryption
 * row rule, the fallback health counter and the RGB565 reference math (make ppa). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ocm_ppa_geom.h"

static int s_fail, s_pass;
#define CHECK(c) do { if (c) s_pass++; else { s_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the CPU compose of mosaico_display.c blit_frame (row memcpy) */
static void cpu_compose(uint16_t *fb, uint16_t *const *pages, int ntiles, int W, int H, int32_t x)
{
    int32_t maxx = (ntiles - 1) * W;
    if (x < 0) x = 0;
    if (x > maxx) x = maxx;
    int a = (int)(x / W), off = (int)(x % W);
    for (int r = 0; r < H; r++) {
        memcpy(fb + r * W, pages[a] + r * W + off, (size_t)(W - off) * 2);
        if (off) memcpy(fb + r * W + (W - off), pages[a + 1] + r * W, (size_t)off * 2);
    }
}

/* a generic 2D window copy, the request the 2D-DMA gets */
static void win_copy(uint16_t *dst, int dst_stride, int dx, const uint16_t *src, int src_stride, int sx, int w, int h)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) dst[r * dst_stride + dx + c] = src[r * src_stride + sx + c];
}

static void test_split(void)
{
    ocm_slide_split_t s = ocm_slide_split(0, 5, 480);
    CHECK(s.a == 0 && s.off == 0 && s.w0 == 480 && s.w1 == 0);
    s = ocm_slide_split(479, 5, 480);
    CHECK(s.a == 0 && s.off == 479 && s.w0 == 1 && s.w1 == 479);
    s = ocm_slide_split(480, 5, 480);
    CHECK(s.a == 1 && s.off == 0 && s.w1 == 0);
    s = ocm_slide_split(-7, 5, 480);
    CHECK(s.a == 0 && s.off == 0);
    s = ocm_slide_split(999999, 5, 480);
    CHECK(s.a == 4 && s.off == 0 && s.w0 == 480 && s.w1 == 0);
    s = ocm_slide_split(300, 1, 480);
    CHECK(s.a == 0 && s.off == 0 && s.w1 == 0);
    s = ocm_slide_split(10, 0, 480);
    CHECK(s.w0 == 0 && s.w1 == 0);
    int bad = 0;
    for (int32_t x = -10; x <= 4 * 480 + 10; x++) {
        s = ocm_slide_split(x, 5, 480);
        if (s.w0 + s.w1 != 480 || s.a < 0 || s.a > 4 || (s.w1 > 0 && s.a + 1 > 4) || s.w0 <= 0) bad++;
    }
    CHECK(bad == 0);

    /* the two window copies of the split give the same frame as the CPU compose, at every scroll position */
    enum { W = 24, H = 5, NT = 4 };
    uint16_t *pages[NT];
    for (int p = 0; p < NT; p++) {
        pages[p] = malloc(W * H * 2);
        for (int i = 0; i < W * H; i++) pages[p][i] = (uint16_t)(p * 1000 + i);
    }
    uint16_t a[W * H], b[W * H];
    int mism = 0;
    for (int32_t x = -3; x <= (NT - 1) * W + 3; x++) {
        cpu_compose(a, pages, NT, W, H, x);
        memset(b, 0xEE, sizeof b);
        s = ocm_slide_split(x, NT, W);
        win_copy(b, W, 0, pages[s.a], W, s.off, s.w0, H);
        if (s.w1) win_copy(b, W, s.w0, pages[s.a + 1], W, 0, s.w1, H);
        if (memcmp(a, b, sizeof a)) mism++;
    }
    CHECK(mism == 0);
    for (int p = 0; p < NT; p++) free(pages[p]);
}

static void test_rebase(void)
{
    ocm_blk_t k;
    uintptr_t buf = 0x48000000u;                       /* 64-aligned, a PSRAM-like address */
    CHECK(ocm_blk_rebase(buf, 960, 2, 480, 432, 64, &k));
    CHECK(k.base == buf && k.off_x == 0 && k.off_y == 0 && k.pic_w == 480 && k.pic_h == 432 && k.buf_size == 480 * 432 * 2);
    CHECK(k.win_lo == buf && k.win_hi == buf + 960 * 432);

    CHECK(ocm_blk_rebase(buf + 14, 960, 2, 100, 3, 64, &k));
    CHECK(k.base == buf && k.off_x == 7 && k.off_y == 0 && k.pic_h == 3);

    /* align 0 (internal RAM, not cached): the block itself is the base */
    CHECK(ocm_blk_rebase(buf + 2 * 33, 960, 2, 40, 40, 0, &k));
    CHECK(k.base == buf + 66 && k.off_x == 0 && k.win_lo == buf + 66 && k.win_hi == buf + 66 + 39 * 960 + 80);

    CHECK(!ocm_blk_rebase(buf, 961, 2, 10, 10, 64, &k));       /* stride not whole pixels */
    CHECK(!ocm_blk_rebase(buf, 960, 2, 481, 10, 64, &k));      /* wider than a row */
    /* align 0: the block is its own base, so the row origin is unknown here; the caller (LVGL) keeps blocks inside rows */
    CHECK(ocm_blk_rebase(buf + 2 * 470, 960, 2, 20, 1, 0, &k) && k.off_x == 0);
    CHECK(!ocm_blk_rebase(buf + 62, 960, 2, 480, 1, 64, &k));  /* re-based x 31 + 480 > 480 */
    CHECK(!ocm_blk_rebase(buf, 2 * 0x4000, 2, 10, 10, 64, &k)); /* 14-bit picture width */
    CHECK(!ocm_blk_rebase(buf, 960, 2, 10, 0x4000, 64, &k));   /* 14-bit picture height */
    CHECK(!ocm_blk_rebase(buf, 960, 2, 10, 10, 48, &k));       /* align not a power of two */
    CHECK(!ocm_blk_rebase(buf, 960, 0, 10, 10, 64, &k));

    /* property: every block that fits a 480 x 480 RGB565 picture rebases back onto itself, the sync window is whole
       cache lines that cover the block and stay inside the picture */
    srand(7);
    int bad = 0, n = 0;
    for (int t = 0; t < 200000; t++) {
        uint32_t w = 1 + rand() % 480, h = 1 + rand() % 480;
        uint32_t x = rand() % (480 - w + 1), y = rand() % (480 - h + 1);
        uintptr_t blk = buf + (uintptr_t)y * 960 + x * 2;
        if (!ocm_blk_rebase(blk, 960, 2, w, h, 64, &k)) { bad++; continue; }
        n++;
        uintptr_t back = k.base + (uintptr_t)k.off_y * 960 + k.off_x * 2;
        uintptr_t end = blk + (uintptr_t)(h - 1) * 960 + w * 2;
        if (back != blk || (k.base & 63) || (k.buf_size & 63) || (k.win_lo & 63) || (k.win_hi & 63) ||
            k.win_lo > blk || k.win_hi < end || k.win_lo < buf || k.win_hi > buf + 960 * 480 ||
            (uint64_t)k.pic_w * k.pic_h * 2 > k.buf_size || k.off_x + w > k.pic_w)
            bad++;
    }
    CHECK(bad == 0 && n == 200000);
}

static void test_region(void)
{
    ocm_region_t r[2] = {{1000, 2000}, {5000, 6000}};
    CHECK(ocm_region_has(r, 2, 1000, 2000));
    CHECK(ocm_region_has(r, 2, 5100, 5200));
    CHECK(!ocm_region_has(r, 2, 1999, 2001));
    CHECK(!ocm_region_has(r, 2, 960, 1100));
    CHECK(!ocm_region_has(r, 2, 3000, 3100));
    CHECK(!ocm_region_has(r, 2, 1500, 1500));
    CHECK(!ocm_region_has(r, 0, 1500, 1600));
    CHECK(!ocm_region_has(NULL, 2, 1500, 1600));
}

static void test_enc(void)
{
    CHECK(ocm_enc_row_ok(480, 480, 0, 2));
    CHECK(ocm_enc_row_ok(480, 8, 8, 2));
    CHECK(!ocm_enc_row_ok(480, 7, 0, 2));
    CHECK(!ocm_enc_row_ok(480, 8, 3, 2));
    CHECK(!ocm_enc_row_ok(476, 8, 0, 2));
    CHECK(ocm_enc_row_ok(4, 4, 4, 4));
}

static void test_health(void)
{
    ocm_hw_health_t h = {0};
    CHECK(ocm_hw_usable(&h));
    ocm_hw_note(&h, OCM_HW_FAILED);
    ocm_hw_note(&h, OCM_HW_FAILED);
    ocm_hw_note(&h, OCM_HW_OK);
    CHECK(ocm_hw_usable(&h) && h.consec == 0 && h.failed == 2 && h.ok == 1);
    for (int i = 0; i < 100; i++) ocm_hw_note(&h, OCM_HW_REFUSED);
    CHECK(ocm_hw_usable(&h) && h.refused == 100);
    ocm_hw_note(&h, OCM_HW_FAILED);
    ocm_hw_note(&h, OCM_HW_REFUSED);
    ocm_hw_note(&h, OCM_HW_FAILED);
    CHECK(ocm_hw_usable(&h));
    ocm_hw_note(&h, OCM_HW_FAILED);
    CHECK(!ocm_hw_usable(&h) && h.consec == OCM_HW_MAX_FAIL);
    ocm_hw_note(&h, OCM_HW_OK);
    CHECK(!ocm_hw_usable(&h));                          /* off stays off until the next boot */
    ocm_hw_health_t g = {0};
    ocm_hw_disable(&g);
    CHECK(!ocm_hw_usable(&g));
    ocm_hw_note(NULL, OCM_HW_FAILED);
}

static void test_color(void)
{
    CHECK(ocm_rgb565_from_888(0xFF, 0x55, 0xAA) == 0xFAB5);   /* esp_driver_ppa test_ppa.cpp fill check: r>>3, g>>2, b>>3 */
    int bad = 0;
    for (uint32_t c = 0; c < (1u << 24); c++) {
        uint8_t r = c >> 16, g = (c >> 8) & 0xFF, b = c & 0xFF;
        uint16_t lv = (uint16_t)(((r & 0xF8) << 8) + ((g & 0xFC) << 3) + ((b & 0xF8) >> 3));   /* LVGL lv_color_to_u16 */
        if (ocm_rgb565_from_888(r, g, b) != lv) bad++;
    }
    CHECK(bad == 0);
    CHECK(ocm_bswap16(0x12AB) == 0xAB12);
    CHECK(ocm_rgb565_close(0xFAB5, 0xFAB5, 0));
    CHECK(ocm_rgb565_close(ocm_rgb565_from_888(0xF8, 0x54, 0xA8), ocm_rgb565_from_888(0xF0, 0x58, 0xA0), 1));
    CHECK(!ocm_rgb565_close(ocm_rgb565_from_888(0xF8, 0x54, 0xA8), ocm_rgb565_from_888(0xE8, 0x54, 0xA8), 1));
    CHECK(!ocm_rgb565_close(0x0000, 0x0002, 1));
}

int main(void)
{
    test_split();
    test_rebase();
    test_region();
    test_enc();
    test_health();
    test_color();
    printf("ppa geom: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
