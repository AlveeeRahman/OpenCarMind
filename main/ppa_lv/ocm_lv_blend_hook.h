/* LVGL 9.5 software-renderer hooks for the ESP-Mosaico (sdkconfig.defaults.esp32s31: CONFIG_LV_DRAW_SW_ASM_CUSTOM=y,
 * CONFIG_LV_DRAW_SW_ASM_CUSTOM_INCLUDE="ocm_lv_blend_hook.h"; main/CMakeLists.txt adds this folder to LVGL's include path).
 * LVGL's lv_draw_sw*.c and blend/lv_draw_sw_blend_to_*.c include this file. Each macro returns LV_RESULT_OK when the
 * hardware did the work and LV_RESULT_INVALID when LVGL's own loop must run (main/ocm_ppa.c). With CONFIG_OCM_PPA_LVGL off
 * this file defines nothing and LVGL builds exactly its plain C path. */
#pragma once
#include "sdkconfig.h"

#if CONFIG_OCM_PPA_LVGL
struct _lv_draw_sw_blend_fill_dsc_t;
struct _lv_draw_sw_blend_image_dsc_t;
int ocm_lv_fill565(struct _lv_draw_sw_blend_fill_dsc_t *dsc);
int ocm_lv_img565(struct _lv_draw_sw_blend_image_dsc_t *dsc);
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc) ocm_lv_fill565(dsc)
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565(dsc) ocm_lv_img565(dsc)
#if CONFIG_OCM_PPA_LVGL_BLEND
int ocm_lv_fill565_opa(struct _lv_draw_sw_blend_fill_dsc_t *dsc);
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565_WITH_OPA(dsc) ocm_lv_fill565_opa(dsc)
#endif
#endif
