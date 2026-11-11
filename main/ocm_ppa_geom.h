/* PPA and 2D-DMA helpers for the ESP-Mosaico screen (CONFIG_OCM_PPA): the rectangle, alignment and fallback rules,
 * kept free of ESP-IDF headers so tests/host can check them (make ppa).
 *
 * Limits taken from ESP-IDF 6.1 for the esp32s31: 2D-DMA descriptor width, height and offsets are 14-bit fields
 * (dma2d_ll.h DMA2D_LL_DESC_2D_FIELD_MAX); PSRAM goes through a 64-byte data cache line (CONFIG_CACHE_L1_DCACHE_LINE_SIZE),
 * internal RAM is not cached; with flash encryption on, PPA rows in PSRAM must be 16-byte aligned
 * (SOC_MEMSPI_ENCRYPTION_ALIGNMENT) and the SRM engine refuses PSRAM buffers outright (ppa_core.c). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OCM_DMA2D_FIELD_MAX 0x3FFFu
#define OCM_MEMSPI_ENC_ALIGN 16u

/* One frame of the launcher slide at scroll position x: page a from column off for w0 pixels goes to x 0, then page a + 1
   from column 0 for w1 pixels goes to x w0 (w1 = 0 when the slide rests on a page). x is clamped to the tile row. */
typedef struct {
    int a;
    int32_t off, w0, w1;
} ocm_slide_split_t;

ocm_slide_split_t ocm_slide_split(int32_t scroll_x, int ntiles, int32_t width);

/* A block inside a pixel buffer, restated as "picture + offset" from an aligned base, which is what the PPA wants for its
   output: base aligned to the cache line, buffer size a whole number of lines. win_lo..win_hi are the cache lines the
   driver's sync touches for this block (the same window ppa_srm.c / ppa_fill.c / ppa_blend.c compute). */
typedef struct {
    uintptr_t base;
    uint32_t pic_w, pic_h;
    uint32_t off_x, off_y;
    uint32_t buf_size;
    uintptr_t win_lo, win_hi;
} ocm_blk_t;

/* align = 0 (not cached) or a power of two. false when the block cannot be expressed (stride not a whole number of
   pixels, block wider than a row, a 14-bit field overflows). */
bool ocm_blk_rebase(uintptr_t block, uint32_t stride_bytes, uint32_t bpp, uint32_t w, uint32_t h, uint32_t align, ocm_blk_t *out);

/* PSRAM buffers the hardware may write. A cache-line sync of a window that reaches outside such a buffer could drop
   another task's dirty data in a shared line, so the LVGL hooks only use the hardware when the window lies inside one. */
typedef struct {
    uintptr_t lo, hi;
} ocm_region_t;

bool ocm_region_has(const ocm_region_t *r, int n, uintptr_t lo, uintptr_t hi);

/* With flash encryption on: picture width, block width and block x offset times the pixel size must each be a multiple
   of 16 bytes for a PSRAM buffer (ppa_check_buffer_alignment). */
bool ocm_enc_row_ok(uint32_t pic_w, uint32_t block_w, uint32_t off_x, uint32_t bpp);

/* Health of one engine. A refused call (the driver said ESP_ERR_INVALID_ARG for this geometry) only sends that call to
   the CPU. A failure (timeout, any other error, a self-test mismatch) counts; OCM_HW_MAX_FAIL in a row turn the engine off
   until the next boot, and every later call goes to the CPU. */
typedef enum { OCM_HW_OK, OCM_HW_REFUSED, OCM_HW_FAILED } ocm_hw_res_t;

typedef struct {
    uint32_t ok, refused, failed, consec;
    bool off;
} ocm_hw_health_t;

#define OCM_HW_MAX_FAIL 3

void ocm_hw_note(ocm_hw_health_t *h, ocm_hw_res_t r);
static inline bool ocm_hw_usable(const ocm_hw_health_t *h) { return !h->off; }
static inline void ocm_hw_disable(ocm_hw_health_t *h) { h->off = true; }

/* RGB565 reference math the self-test compares the hardware against (the same rules LVGL 9.5 uses) */
uint16_t ocm_rgb565_from_888(uint8_t r, uint8_t g, uint8_t b);   /* truncation: r >> 3, g >> 2, b >> 3 */
static inline uint16_t ocm_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
/* true when every channel of a and b is within tol steps (5-bit R/B, 6-bit G) */
bool ocm_rgb565_close(uint16_t a, uint16_t b, int tol);
