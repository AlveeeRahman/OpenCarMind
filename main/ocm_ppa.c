/* PPA and 2D-DMA for the ESP-Mosaico screen (see ocm_ppa.h). Driver rules this follows (ESP-IDF 6.1, esp_driver_ppa and
 * esp_driver_dma for the esp32s31):
 *   - the PPA output buffer and its size must be whole PSRAM cache lines (64 B); internal RAM is not cached;
 *   - the PPA driver writes back its input window and invalidates its output window before it starts. Pixels that share
 *     a cache line with the output window but lie outside the block would be lost if they were dirty, so the hooks write
 *     the window back first and only use buffers registered with ocm_ppa_region_add, which own every line they touch;
 *   - the 2D-DMA copier (esp_async_color_convert, same format = 2D copy) writes back and invalidates the whole
 *     destination picture itself; ocm_ppa_copy565 only gives it cache-line-aligned PSRAM pictures;
 *   - after the hardware wrote a PSRAM window it is invalidated once more, so a line the cache pulled in meanwhile
 *     (prefetch) does not hide the new pixels;
 *   - with flash encryption on, SRM refuses PSRAM buffers and blend/fill rows must be 16-byte aligned. */
#include "sdkconfig.h"
#if CONFIG_OCM_PPA
#include "ocm_ppa.h"
#include "ocm_ppa_geom.h"

#include <inttypes.h>
#include <string.h>
#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_efuse.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if CONFIG_OCM_PPA_LVGL
#include "lvgl.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "ppa_lv/ocm_lv_blend_hook.h"
#endif

static const char *TAG = "ppa";

#if CONFIG_OCM_PPA_LVGL
#define OCM_LV_HOOKS "on"
#else
#define OCM_LV_HOOKS "off"
#endif

static ppa_client_handle_t s_srm, s_fill;
#if CONFIG_OCM_PPA_LVGL_BLEND
static ppa_client_handle_t s_blend;
#endif
static async_color_convert_handle_t s_cc;
static SemaphoreHandle_t s_cc_done;
static ocm_hw_health_t s_h_dma, s_h_srm, s_h_fill, s_h_blend;
static size_t s_ext_align;                              /* the PSRAM line size, for the log line */
static bool s_enc, s_ready;

#define OCM_PPA_REGIONS 8
static ocm_region_t s_reg[OCM_PPA_REGIONS];
static int s_nreg;

static struct {
    uint32_t copy_dma, copy_srm, copy_cpu;
    uint32_t lv_fill_hw, lv_fill_cpu, lv_img_hw, lv_img_cpu, lv_blend_hw, lv_blend_cpu;
} s_n, s_n_logged;

static ocm_hw_res_t res_of(esp_err_t e) { return e == ESP_OK ? OCM_HW_OK : e == ESP_ERR_INVALID_ARG ? OCM_HW_REFUSED : OCM_HW_FAILED; }
static bool dma_addr_ok(const void *p) { return esp_ptr_dma_capable(p) || esp_ptr_external_ram(p); }   /* not flash-mapped data */
static size_t align_for(const void *p) { return esp_cache_get_line_size_by_addr(p); }   /* 0 = not cached */

static void sync_window(uintptr_t lo, uintptr_t hi, int flags)
{
    if (hi > lo) esp_cache_msync((void *)lo, hi - lo, flags);
}

/* the cache lines of rows dy..dy+h-1, columns dx..dx+w-1 of a picture with the given stride (pixels) */
static void window_of(const void *pic, uint32_t stride, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, size_t a,
                      uintptr_t *lo, uintptr_t *hi)
{
    uintptr_t s = (uintptr_t)pic + ((uintptr_t)dy * stride + dx) * 2;
    uintptr_t e = (uintptr_t)pic + ((uintptr_t)(dy + h - 1) * stride + dx + w) * 2;
    *lo = a ? s & ~(uintptr_t)(a - 1) : s;
    *hi = a ? (e + a - 1) & ~(uintptr_t)(a - 1) : e;
}

#if CONFIG_DMA2D_ISR_IRAM_SAFE
#define OCM_CC_ATTR IRAM_ATTR
#else
#define OCM_CC_ATTR
#endif
static bool OCM_CC_ATTR cc_done_cb(async_color_convert_handle_t h, async_color_convert_event_data_t *ed, void *arg)
{
    (void)h; (void)ed; (void)arg;
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_cc_done, &woke);
    return woke == pdTRUE;
}

static ocm_hw_res_t dma_copy(void *dst, uint32_t dst_stride, uint32_t dst_h, uint32_t dx, uint32_t dy,
                             const void *src, uint32_t src_stride, uint32_t src_h, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
    const async_color_convert_request_t rq = {
        .src_buffer = src, .src_stride = src_stride, .src_height = src_h, .src_x = sx, .src_y = sy,
        .dst_buffer = dst, .dst_stride = dst_stride, .dst_height = dst_h, .dst_x = dx, .dst_y = dy,
        .copy_width = w, .copy_height = h,
        .src_color_format = ESP_COLOR_FOURCC_RGB16, .dst_color_format = ESP_COLOR_FOURCC_RGB16,
    };
    esp_err_t e = esp_async_color_convert(s_cc, &rq, cc_done_cb, NULL);
    if (e != ESP_OK) return res_of(e);
    if (xSemaphoreTake(s_cc_done, pdMS_TO_TICKS(200)) != pdTRUE) {
        /* the transfer may still land later: never use this copier again, the CPU does every later frame */
        ESP_LOGE(TAG, "2D-DMA copy did not finish in 200 ms, the CPU copies from now on");
        ocm_hw_disable(&s_h_dma);
        return OCM_HW_FAILED;
    }
    return OCM_HW_OK;
}

static ocm_hw_res_t srm_copy(void *dst, uint32_t dst_stride, uint32_t dst_h, uint32_t dx, uint32_t dy,
                             const void *src, uint32_t src_stride, uint32_t src_h, uint32_t sx, uint32_t sy,
                             uint32_t w, uint32_t h, uint32_t dst_buf_size, bool byte_swap)
{
    ppa_srm_oper_config_t c = {0};
    c.in.buffer = src;
    c.in.pic_w = src_stride;
    c.in.pic_h = src_h;
    c.in.block_w = w;
    c.in.block_h = h;
    c.in.block_offset_x = sx;
    c.in.block_offset_y = sy;
    c.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    c.out.buffer = dst;
    c.out.buffer_size = dst_buf_size;
    c.out.pic_w = dst_stride;
    c.out.pic_h = dst_h;
    c.out.block_offset_x = dx;
    c.out.block_offset_y = dy;
    c.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    c.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    c.scale_x = 1.0f;
    c.scale_y = 1.0f;
    c.byte_swap = byte_swap;
    c.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    c.mode = PPA_TRANS_MODE_BLOCKING;
    return res_of(ppa_do_scale_rotate_mirror(s_srm, &c));
}

bool ocm_ppa_copy565(void *dst, uint32_t dst_stride, uint32_t dst_h, uint32_t dx, uint32_t dy,
                     const void *src, uint32_t src_stride, uint32_t src_h, uint32_t sx, uint32_t sy,
                     uint32_t w, uint32_t h, bool byte_swap)
{
    if (!s_ready || !dst || !src || w == 0 || h == 0) goto cpu;
    if (sx + w > src_stride || sy + h > src_h || dx + w > dst_stride || dy + h > dst_h) goto cpu;
    if (!dma_addr_ok(dst) || !dma_addr_ok(src)) goto cpu;
    size_t a = align_for(dst);
    uint32_t dst_size = dst_stride * dst_h * 2;
    if (a && (((uintptr_t)dst & (a - 1)) || (dst_size & (a - 1)))) goto cpu;
    bool done = false;
    bool psram_side = esp_ptr_external_ram(dst) || esp_ptr_external_ram(src);
    /* encrypted PSRAM needs 16-byte rows and offsets; the 2D-DMA copier does not check this itself (it would write wrong
       pixels instead of refusing), so the rule is checked here */
    bool enc_rows_ok = !s_enc || !psram_side || (ocm_enc_row_ok(src_stride, w, sx, 2) && ocm_enc_row_ok(dst_stride, w, dx, 2));
    if (!byte_swap && enc_rows_ok && ocm_hw_usable(&s_h_dma)) {
        ocm_hw_res_t r = dma_copy(dst, dst_stride, dst_h, dx, dy, src, src_stride, src_h, sx, sy, w, h);
        ocm_hw_note(&s_h_dma, r);
        if (r == OCM_HW_OK) { done = true; s_n.copy_dma++; }
    }
    if (!done && s_srm && ocm_hw_usable(&s_h_srm) && !(s_enc && psram_side)) {
        if (a) {   /* the driver invalidates the output window: dirty pixels beside the block in those lines go to memory first */
            uintptr_t lo, hi;
            window_of(dst, dst_stride, dx, dy, w, h, a, &lo, &hi);
            sync_window(lo, hi, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        ocm_hw_res_t r = srm_copy(dst, dst_stride, dst_h, dx, dy, src, src_stride, src_h, sx, sy, w, h, dst_size, byte_swap);
        ocm_hw_note(&s_h_srm, r);
        if (r == OCM_HW_OK) { done = true; s_n.copy_srm++; }
    }
    if (done) {
        if (a) {
            uintptr_t lo, hi;
            window_of(dst, dst_stride, dx, dy, w, h, a, &lo, &hi);
            sync_window(lo, hi, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }
        return true;
    }
cpu:
    s_n.copy_cpu++;
    return false;
}

bool ocm_ppa_region_add(const void *p, size_t n)
{
    size_t a = p ? align_for(p) : 0;
    if (!n || !esp_ptr_external_ram(p) || !a) return false;
    if (((uintptr_t)p & (a - 1)) || (n & (a - 1))) return false;
    for (int i = 0; i < s_nreg; i++)
        if (s_reg[i].lo == (uintptr_t)p) { s_reg[i].hi = (uintptr_t)p + n; return true; }
    if (s_nreg >= OCM_PPA_REGIONS) return false;
    s_reg[s_nreg++] = (ocm_region_t){(uintptr_t)p, (uintptr_t)p + n};
    return true;
}

/* ---------- fills and blends on an arbitrary block (the LVGL hooks and the self-test) ---------- */
static bool hw_target(void *dest, int32_t stride, int32_t w, int32_t h, ocm_blk_t *k, bool *psram)
{
    if (w <= 0 || h <= 0 || stride <= 0 || !dma_addr_ok(dest)) return false;
    *psram = esp_ptr_external_ram(dest);
    size_t a = align_for(dest);
    if (!ocm_blk_rebase((uintptr_t)dest, (uint32_t)stride, 2, (uint32_t)w, (uint32_t)h, (uint32_t)a, k)) return false;
    if (a && !ocm_region_has(s_reg, s_nreg, k->win_lo, k->win_hi)) return false;
    if (*psram && s_enc && !ocm_enc_row_ok(k->pic_w, (uint32_t)w, k->off_x, 2)) return false;
    return true;
}

static void out_of(ppa_out_pic_blk_config_t *o, const ocm_blk_t *k)
{
    o->buffer = (void *)k->base;
    o->buffer_size = k->buf_size;
    o->pic_w = k->pic_w;
    o->pic_h = k->pic_h;
    o->block_offset_x = k->off_x;
    o->block_offset_y = k->off_y;
}

static bool hw_fill565(void *dest, int32_t stride, int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b)
{
    ocm_blk_t k;
    bool psram;
    if (!s_fill || !ocm_hw_usable(&s_h_fill) || !hw_target(dest, stride, w, h, &k, &psram)) return false;
    ppa_fill_oper_config_t f = {0};
    out_of(&f.out, &k);
    f.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
    f.fill_block_w = (uint32_t)w;
    f.fill_block_h = (uint32_t)h;
    f.fill_argb_color.a = 0xFF;
    f.fill_argb_color.r = r;
    f.fill_argb_color.g = g;
    f.fill_argb_color.b = b;
    f.mode = PPA_TRANS_MODE_BLOCKING;
    if (psram) sync_window(k.win_lo, k.win_hi, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ocm_hw_res_t res = res_of(ppa_do_fill(s_fill, &f));
    ocm_hw_note(&s_h_fill, res);
    if (psram && res == OCM_HW_OK) sync_window(k.win_lo, k.win_hi, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return res == OCM_HW_OK;
}

#if CONFIG_OCM_PPA_LVGL_BLEND
/* colour over the block at opacity opa: fg is an A8 picture whose alpha is replaced by opa, so its bytes are never used;
   it reads the block's own memory, which is valid DMA memory of the right size */
static bool hw_blend565(void *dest, int32_t stride, int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t opa)
{
    ocm_blk_t k;
    bool psram;
    if (!s_blend || !ocm_hw_usable(&s_h_blend) || !hw_target(dest, stride, w, h, &k, &psram)) return false;
    ppa_blend_oper_config_t c = {0};
    c.in_bg.buffer = (const void *)k.base;
    c.in_bg.pic_w = k.pic_w;
    c.in_bg.pic_h = k.pic_h;
    c.in_bg.block_w = (uint32_t)w;
    c.in_bg.block_h = (uint32_t)h;
    c.in_bg.block_offset_x = k.off_x;
    c.in_bg.block_offset_y = k.off_y;
    c.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
    c.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    c.bg_alpha_fix_val = 0xFF;
    c.in_fg.buffer = (const void *)k.base;
    c.in_fg.pic_w = k.pic_w * 2;
    c.in_fg.pic_h = k.pic_h;
    c.in_fg.block_w = (uint32_t)w;
    c.in_fg.block_h = (uint32_t)h;
    c.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_A8;
    c.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    c.fg_alpha_fix_val = opa;
    c.fg_fix_rgb_val.r = r;
    c.fg_fix_rgb_val.g = g;
    c.fg_fix_rgb_val.b = b;
    out_of(&c.out, &k);
    c.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
    c.mode = PPA_TRANS_MODE_BLOCKING;
    if (psram) sync_window(k.win_lo, k.win_hi, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ocm_hw_res_t res = res_of(ppa_do_blend(s_blend, &c));
    ocm_hw_note(&s_h_blend, res);
    if (psram && res == OCM_HW_OK) sync_window(k.win_lo, k.win_hi, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return res == OCM_HW_OK;
}
#endif

/* ---------- LVGL 9.5 software-renderer hooks (LV_DRAW_SW_ASM_CUSTOM) ---------- */
#if CONFIG_OCM_PPA_LVGL
int ocm_lv_fill565(struct _lv_draw_sw_blend_fill_dsc_t *d)
{
    if (s_ready && (uint32_t)d->dest_w * (uint32_t)d->dest_h >= CONFIG_OCM_PPA_MIN_PX &&
        hw_fill565(d->dest_buf, d->dest_stride, d->dest_w, d->dest_h, d->color.red, d->color.green, d->color.blue)) {
        s_n.lv_fill_hw++;
        return LV_RESULT_OK;
    }
    s_n.lv_fill_cpu++;
    return LV_RESULT_INVALID;
}

int ocm_lv_img565(struct _lv_draw_sw_blend_image_dsc_t *d)
{
    ocm_blk_t k;
    bool psram;
    int32_t w = d->dest_w, h = d->dest_h;
    if (!s_ready || !s_srm || !ocm_hw_usable(&s_h_srm) || (uint32_t)w * (uint32_t)h < CONFIG_OCM_PPA_MIN_PX) goto cpu;
    if (d->src_color_format != LV_COLOR_FORMAT_RGB565 || d->blend_mode != LV_BLEND_MODE_NORMAL) goto cpu;
    if (!dma_addr_ok(d->src_buf) || d->src_stride <= 0 || (d->src_stride & 1) || d->src_stride / 2 > (int32_t)OCM_DMA2D_FIELD_MAX) goto cpu;
    if (!hw_target(d->dest_buf, d->dest_stride, w, h, &k, &psram)) goto cpu;
    if (s_enc && (psram || esp_ptr_external_ram(d->src_buf))) goto cpu;   /* SRM refuses PSRAM with flash encryption */
    if (psram) sync_window(k.win_lo, k.win_hi, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ocm_hw_res_t r = srm_copy((void *)k.base, k.pic_w, k.pic_h, k.off_x, k.off_y, d->src_buf, (uint32_t)d->src_stride / 2, (uint32_t)h,
                              0, 0, (uint32_t)w, (uint32_t)h, k.buf_size, false);
    ocm_hw_note(&s_h_srm, r);
    if (r == OCM_HW_OK) {
        if (psram) sync_window(k.win_lo, k.win_hi, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        s_n.lv_img_hw++;
        return LV_RESULT_OK;
    }
cpu:
    s_n.lv_img_cpu++;
    return LV_RESULT_INVALID;
}

#if CONFIG_OCM_PPA_LVGL_BLEND
int ocm_lv_fill565_opa(struct _lv_draw_sw_blend_fill_dsc_t *d)
{
    if (s_ready && (uint32_t)d->dest_w * (uint32_t)d->dest_h >= CONFIG_OCM_PPA_MIN_PX &&
        hw_blend565(d->dest_buf, d->dest_stride, d->dest_w, d->dest_h, d->color.red, d->color.green, d->color.blue, d->opa)) {
        s_n.lv_blend_hw++;
        return LV_RESULT_OK;
    }
    s_n.lv_blend_cpu++;
    return LV_RESULT_INVALID;
}
#endif
#endif

/* ---------- boot self-test: each engine against the CPU on small buffers; a mismatch turns that engine off ---------- */
#define ST_W 64
#define ST_H 16
static bool st_expect(const char *what, const uint16_t *got, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                      uint16_t (*want)(uint32_t x, uint32_t y, const void *ctx), const void *ctx, int tol)
{
    for (uint32_t y = 0; y < ST_H; y++)
        for (uint32_t x = 0; x < ST_W; x++) {
            bool in = x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
            uint16_t g = got[y * ST_W + x], e = in ? want(x - x0, y - y0, ctx) : 0xEEEE;
            if (in ? !ocm_rgb565_close(g, e, tol) : g != e) {
                ESP_LOGW(TAG, "self-test: %s pixel (%" PRIu32 ",%" PRIu32 ") is 0x%04x, expected 0x%04x: the CPU does it", what, x, y, g, e);
                return false;
            }
        }
    return true;
}

static uint16_t st_src_px(uint32_t x, uint32_t y) { return (uint16_t)(x * 977u + y * 131u + 0x1234u); }
typedef struct { uint32_t sx, sy; bool swap; } st_copy_ctx_t;
static uint16_t st_copy_want(uint32_t x, uint32_t y, const void *c)
{
    const st_copy_ctx_t *k = c;
    uint16_t v = st_src_px(x + k->sx, y + k->sy);
    return k->swap ? ocm_bswap16(v) : v;
}
static uint16_t st_fill_want(uint32_t x, uint32_t y, const void *c) { (void)x; (void)y; return *(const uint16_t *)c; }
#if CONFIG_OCM_PPA_LVGL_BLEND
typedef struct { uint8_t r, g, b, opa; } st_blend_ctx_t;
static uint16_t st_blend_want(uint32_t x, uint32_t y, const void *c)
{
    const st_blend_ctx_t *k = c;
    uint16_t bg = st_src_px(x, y);
    int br = bg >> 11, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int fr = k->r >> 3, fg = k->g >> 2, fb = k->b >> 3, a = k->opa;
    int r = (fr * a + br * (255 - a) + 127) / 255, g = (fg * a + bgc * (255 - a) + 127) / 255, b = (fb * a + bb * (255 - a) + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
#endif

static void st_reset(uint16_t *p) { for (int i = 0; i < ST_W * ST_H; i++) p[i] = 0xEEEE; }

static void self_test(void)
{
    const size_t n = ST_W * ST_H * 2;
    uint16_t *src = heap_caps_aligned_alloc(64, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    uint16_t *dst = heap_caps_aligned_alloc(64, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    uint16_t *idst = heap_caps_aligned_alloc(64, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!src || !dst || !idst) {
        ESP_LOGW(TAG, "self-test: no memory, the CPU does everything");
        ocm_hw_disable(&s_h_dma); ocm_hw_disable(&s_h_srm); ocm_hw_disable(&s_h_fill); ocm_hw_disable(&s_h_blend);
        goto out;
    }
    s_ext_align = align_for(src);
    for (uint32_t y = 0; y < ST_H; y++)
        for (uint32_t x = 0; x < ST_W; x++) src[y * ST_W + x] = st_src_px(x, y);
    s_ready = true;   /* the copy and fill paths below are the real ones */

    /* 2D-DMA window copy, PSRAM to PSRAM, odd offsets; the pixels around the window must stay as they were */
    st_copy_ctx_t cc = {5, 2, false};
    st_reset(dst);
    uint32_t dma_before = s_n.copy_dma;
    if (!ocm_hw_usable(&s_h_dma) || !ocm_ppa_copy565(dst, ST_W, ST_H, 9, 3, src, ST_W, ST_H, cc.sx, cc.sy, 41, 10, false) ||
        s_n.copy_dma == dma_before || !st_expect("2D-DMA copy", dst, 9, 3, 41, 10, st_copy_want, &cc, 0))
        ocm_hw_disable(&s_h_dma);

    /* SRM byte swap, PSRAM to PSRAM (the page pictures) */
    cc.swap = true;
    st_reset(dst);
    uint32_t srm_before = s_n.copy_srm;
    if (!s_srm || !ocm_ppa_copy565(dst, ST_W, ST_H, 3, 1, src, ST_W, ST_H, cc.sx, cc.sy, 50, 12, true) || s_n.copy_srm == srm_before ||
        !st_expect("SRM byte swap", dst, 3, 1, 50, 12, st_copy_want, &cc, 0))
        ocm_hw_disable(&s_h_srm);

    /* fill: internal RAM (the LVGL draw buffers) and a registered PSRAM picture with an unaligned block */
    uint16_t want = ocm_rgb565_from_888(0xFF, 0x55, 0xAA);
    st_reset(idst);
    bool fill_ok = hw_fill565(idst + 2 * ST_W + 7, ST_W * 2, 30, 9, 0xFF, 0x55, 0xAA) && st_expect("fill (internal)", idst, 7, 2, 30, 9, st_fill_want, &want, 0);
    int nreg = s_nreg;
    bool reg = ocm_ppa_region_add(dst, n);
    if (fill_ok && !reg) fill_ok = false;
    if (fill_ok) {
        st_reset(dst);
        fill_ok = hw_fill565(dst + 4 * ST_W + 13, ST_W * 2, 21, 7, 0xFF, 0x55, 0xAA) && st_expect("fill (PSRAM)", dst, 13, 4, 21, 7, st_fill_want, &want, 0);
    }
    if (!fill_ok) ocm_hw_disable(&s_h_fill);
#if CONFIG_OCM_PPA_LVGL_BLEND
    st_blend_ctx_t bc = {0x30, 0x90, 0xE0, 96};
    memcpy(dst, src, n);
    bool blend_ok = reg && hw_blend565(dst + 6 * ST_W + 11, ST_W * 2, 33, 8, bc.r, bc.g, bc.b, bc.opa);
    if (blend_ok)
        for (uint32_t y = 0; y < ST_H && blend_ok; y++)
            for (uint32_t x = 0; x < ST_W && blend_ok; x++) {
                bool in = x >= 11 && x < 44 && y >= 6 && y < 14;
                uint16_t g = dst[y * ST_W + x], e = in ? st_blend_want(x, y, &bc) : st_src_px(x, y);
                if (in ? !ocm_rgb565_close(g, e, 2) : g != e) {
                    ESP_LOGW(TAG, "self-test: blend pixel (%" PRIu32 ",%" PRIu32 ") is 0x%04x, expected 0x%04x: the CPU does it", x, y, g, e);
                    blend_ok = false;
                }
            }
    if (!blend_ok) ocm_hw_disable(&s_h_blend);
#endif
    s_nreg = nreg;   /* the test buffer is freed below */
out:
    heap_caps_free(src);
    heap_caps_free(dst);
    heap_caps_free(idst);
    memset(&s_n, 0, sizeof s_n);
}

void ocm_ppa_init(void)
{
    if (s_ready || s_cc_done) return;
    s_enc = esp_efuse_is_flash_encryption_enabled();
    const ppa_client_config_t srm = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    const ppa_client_config_t fil = { .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1 };
    if (ppa_register_client(&srm, &s_srm) != ESP_OK) { s_srm = NULL; ocm_hw_disable(&s_h_srm); }
    if (ppa_register_client(&fil, &s_fill) != ESP_OK) { s_fill = NULL; ocm_hw_disable(&s_h_fill); }
#if CONFIG_OCM_PPA_LVGL_BLEND
    const ppa_client_config_t bl = { .oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 1 };
    if (ppa_register_client(&bl, &s_blend) != ESP_OK) { s_blend = NULL; ocm_hw_disable(&s_h_blend); }
#else
    ocm_hw_disable(&s_h_blend);
#endif
    s_cc_done = xSemaphoreCreateBinary();
    const async_color_convert_config_t ccfg = { .backlog = 1 };
    if (!s_cc_done || esp_async_color_convert_install_dma2d(&ccfg, &s_cc) != ESP_OK) { s_cc = NULL; ocm_hw_disable(&s_h_dma); }
    self_test();
    ESP_LOGI(TAG, "PPA and 2D-DMA: copy %s, byte swap %s, fill %s, blend %s, LVGL hooks %s (from %d px); PSRAM line %u B, flash encryption %s",
             ocm_hw_usable(&s_h_dma) ? "2D-DMA" : "CPU", ocm_hw_usable(&s_h_srm) ? "PPA" : "CPU",
             ocm_hw_usable(&s_h_fill) ? "PPA" : "CPU", ocm_hw_usable(&s_h_blend) ? "PPA" : "CPU",
             OCM_LV_HOOKS, CONFIG_OCM_PPA_MIN_PX, (unsigned)s_ext_align, s_enc ? "on" : "off");
}

void ocm_ppa_log(void)
{
    if (!memcmp(&s_n, &s_n_logged, sizeof s_n)) return;
    s_n_logged = s_n;
    ESP_LOGI(TAG, "slide copies dma %" PRIu32 " ppa %" PRIu32 " cpu %" PRIu32 "; lvgl fill %" PRIu32 "/%" PRIu32 " image %" PRIu32 "/%" PRIu32
             " blend %" PRIu32 "/%" PRIu32 " (hw/cpu); failures dma %" PRIu32 " ppa %" PRIu32 "%s",
             s_n.copy_dma, s_n.copy_srm, s_n.copy_cpu, s_n.lv_fill_hw, s_n.lv_fill_cpu, s_n.lv_img_hw, s_n.lv_img_cpu,
             s_n.lv_blend_hw, s_n.lv_blend_cpu, s_h_dma.failed, s_h_srm.failed + s_h_fill.failed + s_h_blend.failed,
             (s_h_dma.off || s_h_srm.off || s_h_fill.off) ? " (an engine is off: CPU)" : "");
}
#endif
