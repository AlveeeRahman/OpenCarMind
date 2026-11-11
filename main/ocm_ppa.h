/* PPA and 2D-DMA for the ESP-Mosaico screen (CONFIG_OCM_PPA, esp32s31 only).
 *
 * What runs on the hardware, with the CPU path kept as the fallback for every call:
 *   - the launcher slide: each frame's compose (two window copies from the page pictures into a PSRAM frame) by 2D-DMA,
 *     and the byte swap of each page picture into panel order by the PPA scale-rotate-mirror engine (scale 1, byte_swap);
 *   - with CONFIG_OCM_PPA_LVGL, inside LVGL's software renderer: large opaque RGB565 fills (PPA fill) and opaque RGB565
 *     image copies (PPA SRM), through LVGL's LV_DRAW_SW_ASM_CUSTOM hooks (main/ppa_lv/ocm_lv_blend_hook.h);
 *   - with CONFIG_OCM_PPA_LVGL_BLEND, semi-transparent fills (PPA blend).
 * A call the driver refuses goes to the CPU. Three failures in a row, or a self-test mismatch at boot, turn that engine
 * off until the next boot. All callers are the LVGL task. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

#if CONFIG_OCM_PPA
/* registers the PPA clients and the 2D-DMA copier and checks each against the CPU on small buffers */
void ocm_ppa_init(void);

/* RGB565 window copy: w x h pixels from src (stride and height in pixels) at (sx, sy) to dst at (dx, dy). byte_swap swaps
   the two bytes of every pixel (panel order). A PSRAM dst must start on a cache line and be a whole number of lines.
   false = nothing was written, do it on the CPU. */
bool ocm_ppa_copy565(void *dst, uint32_t dst_stride, uint32_t dst_h, uint32_t dx, uint32_t dy,
                     const void *src, uint32_t src_stride, uint32_t src_h, uint32_t sx, uint32_t sy,
                     uint32_t w, uint32_t h, bool byte_swap);

/* a PSRAM buffer the LVGL hooks may let the hardware write (cache-line aligned start and size); false if not taken */
bool ocm_ppa_region_add(const void *p, size_t n);

/* one "ppa:" log line with the hardware and CPU counts, when they changed since the last call */
void ocm_ppa_log(void);
#endif
