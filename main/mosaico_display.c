/* ESP-Mosaico screen (ESP32-S31 only): 2.16" CO5300 AMOLED, 480x480 over QSPI, CST9220 touch, LVGL 9 through
 * esp_lvgl_port. Panel and touch bring-up follow the ESP-Mosaico bring-up display and touch code (layers 1-2, run on
 * this board). This file is the launcher shell; the apps are in mosaico_apps.c (mosaico_ui.h). It implements display.h,
 * so app_main.c and the rest of the car firmware are unchanged.
 *
 * Soft real-time task set on the ESP-Mosaico (rate-monotonic order: the shorter the period, the higher the priority,
 * except that the screen ranks below the model on purpose: a late frame is harmless, a late model window is a lost one).
 * Periods and deadlines are the ones the jobs register with sched_mon (sched_register), which counts runs, misses
 * (run time over the deadline), worst run time and CPU per run; the Board app shows them.
 *
 *   task        job            period        deadline   prio  core  stack           notes
 *   obd         obd_poll       1 s (rows)    30 s       5     1     14 KB internal  ELM327 I/O, collectors, R.E.V physics
 *   mode        -              event         -          5     0     3 KB            DRIVE / SEEK arbitration
 *   sync        -              event         -          4     0     8 KB            Wi-Fi clock
 *   ml          ml             10 s windows  10 s       3     1     6 KB PSRAM      E.V or R.E.V, one at a time
 *   taskLVGL    ui             50 ms         50 ms      2     0     8 KB internal   this file: visible screen only
 *   rev         rev            stride        30 s       2     1     6 KB            fuel learner
 *   sdsync      -              5 s           -          2     any   6 KB            card / flash FAT saves
 *   usb_door    -              event         -          2     any   6 KB            OCM1 protocol on the CDC port
 *   board       -              2 s           -          1     0     3 KB internal   gauge, FAT free space, NVS saves
 *   cpumon      -              1 s           -          1     0     3 KB            CPU load, heap
 *   sched       -              1 s           -          1     0     4 KB            the monitor itself
 *
 * UI rules: the LVGL task only reads published snapshots (status mutex held for a memcpy, the others a spinlock); the
 * flags other tasks set (display_next_page and friends) are plain volatile words; nothing here allocates in the
 * periodic path (the snapshot and the format buffer are static, in PSRAM); NVS writes go to the board task. */
#include "sdkconfig.h"
#include <math.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "board.h"
#include "display.h"
#include "mosaico_ui.h"
#include "mosaico_board.h"
#include "mosaico_clock.h"
#include "sched_mon.h"
#include "remote_config.h"
#include "elm327.h"
#include "mosaico_drive.h"
#include "sdlog.h"
#include "fault_engine.h"
#include "econ.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst9220.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_task_wdt.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_OCM_SWIPE_INTENT
#include <stdint.h>
#include "swipe_intent.h"
#include "esp_app_desc.h"
#include "esp_cpu.h"
#include "src/indev/lv_indev_scroll.h"                /* lv_indev_scroll_throw_handler / _predict */
#endif

#if CONFIG_OCM_UI_METRICS
#include "esp_app_desc.h"
#include "esp_cpu.h"
#include "esp_random.h"
#endif

static const char *TAG = "display";

/* ---------- LVGL heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC): widgets, styles and labels never take internal RAM ---------- */
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { (void)mem; (void)bytes; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }
void *lv_malloc_core(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *lv_realloc_core(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void lv_free_core(void *p) { heap_caps_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t *mon) { (void)mon; }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

/* ---------- themes: one table per palette. Dark = the owner's chosen looks (A "Night drive", B "Pixel garage" for Drive,
   C "Cockpit" for home); Light = the Row I light look of the launcher carried into every app page. Every text colour
   is >= 4.5:1 (WCAG 2.x AA) on each ground it is drawn on, in both palettes: tests/host/test_ui_theme.c reads these
   tables and checks the pairs. Class colours: stopped, braking, accelerating, cruising. ---------- */
#define THEME_FONTS \
    .f_speed = &lv_font_montserrat_48, .f_num_b = &lv_font_montserrat_48, .f_hero = &lv_font_montserrat_48, \
    .f_big = &lv_font_montserrat_32, .f_large = &lv_font_montserrat_28, .f_title = &lv_font_montserrat_24, \
    .f_value = &lv_font_montserrat_20, .f_status = &lv_font_montserrat_18, .f_body = &lv_font_montserrat_16, \
    .f_label = &lv_font_montserrat_14, .f_small = &lv_font_montserrat_12
#define THEME_SHAPES \
    .radius_card = 24, .radius_row = 16, .radius_button = 14, .radius_tile_b = 10, .border_card = 1, .border_accent = 2, \
    .pad_card = 14, .shadow_b = 5

static const ui_theme_t THEME_DARK = {
    .name = "dark",
    .bg = 0x000000, .card = 0x0B0B0D, .border = 0x2A2A2E, .text = 0xF2ECE1, .muted = 0xB8B8B8, .dim = 0x4A4A4A,
    .track = 0x1A1A1D, .good = 0x56F487, .warn = 0xFCCA21, .bad = 0xEA4B4B, .estimate = 0xF2A13A, .ai = 0xEA4B4B,
    .scrim = 0x000000, .banner = 0x16161A,
    .section = { [SEC_SKY] = 0x8CC8FF, [SEC_ORANGE] = 0xFE860F, [SEC_GREEN] = 0x56F487, [SEC_PINK] = 0xF472B6,
                 [SEC_AMBER] = 0xF2A13A, [SEC_RED] = 0xFF2A3D, [SEC_AQUA] = 0x72ECE4, [SEC_VIOLET] = 0xAD8DFD,
                 [SEC_CYAN] = 0x38B8F6 },
    .cls = { 0x9372C8, 0xE05B4A, 0x4E82E5, 0xC68206 },
    .icon = 0xF2ECE1, .icon_glyph = 0x111111,
    .light = false, .panel = 0x111216, .panel_edge = 0x1F2026, .inset = 0x1A1B20, .inset_hi = 0x26272D, .chip = 0x141519,
    .label2 = 0x8E8E93, .off = 0x4A4A4A, .sheet = 0x16161A, .press_tint = 0xFFFFFF, .tile_glyph = 0x0B0B0D,
    .dcls = { 0xAD8DFD, 0xFD5A4A, 0x5AA2FF, 0xFCCA21 },
    /* black ground on the AMOLED (v5's #16212F rendered bright blue): near-black tiles, 1 px border, dark hard shadow */
    .b_ground = 0x000000, .b_tile = 0x0B0B0D, .b_border = 0x2A2A2E, .b_shadow = 0x1A1A1D, .b_lit = 0x7AA2FF, .b_lit_hi = 0xF2C94C,
    .b_unlit = 0x222226, .b_tank = 0x7CCB6E, .b_face_tile = 0xF2ECE1, .b_face_text = 0x111111, .b_head = 0x111111,
    .b_tag_model = 0xF2A13A, .b_tag_phys = 0xF2A13A, .b_tag_learn = 0x7CCB6E, .b_text = 0xF2ECE1, .b_muted = 0xB8B8B8,
    .c_bg = 0x000000, .c_track = 0x1A1A1D, .c_rpm = 0x8CC8FF, .c_tick = 0x4A4A4A, .c_status = 0xB8B8B8,
    .c_btn = 0xF2ECE1, .c_btn_glyph = 0x111111, .c_text = 0xF2ECE1, .c_muted = 0xB8B8B8,
    THEME_SHAPES, THEME_FONTS,
    .face_top = 0xF2ECE1, .face_bot = 0xF2ECE1, .face_tick = 0xFE860F, .face_dot = 0xB9B2A6, .face_hour = 0xFE860F, .face_min = 0x17212E,
    .sb_cpu = 0x72ECE4, .sb_ai = 0xFE860F, .sb_mips = 0xF2ECE1, .sb_pwr = 0x56F487,
};

/* Light (T1, owner 2026-11-11: "white mode app internals need to be white too"): the launcher's Row I light ground
   (#F3F1EC, white cards) on every app page. Accents are the dark ones darkened until they pass 4.5:1 on the button
   fill (track), which also covers the page and the cards, and so that the page colour passes on them (a selected
   segment, the Save button). Amber and green in particular are deep (#845400, #186A2C): the dark ones fail on white. */
static const ui_theme_t THEME_LIGHT = {
    .name = "light",
    .bg = 0xF3F1EC, .card = 0xFFFFFF, .border = 0xD9D6CF, .text = 0x1C1C1E, .muted = 0x5C5C61, .dim = 0xB8B5AE,
    .track = 0xE8E5DE, .good = 0x186A2C, .warn = 0x845400, .bad = 0xB8121C, .estimate = 0x8A4F00, .ai = 0xB8121C,
    .scrim = 0x1C1C1E, .banner = 0xFFFFFF,
    .section = { [SEC_SKY] = 0x1A5BC4, [SEC_ORANGE] = 0xA34200, [SEC_GREEN] = 0x186A2C, [SEC_PINK] = 0xA81D5A,
                 [SEC_AMBER] = 0x845000, [SEC_RED] = 0xB8101C, [SEC_AQUA] = 0x0A665F, [SEC_VIOLET] = 0x5B2BB5,
                 [SEC_CYAN] = 0x0A6584 },
    .cls = { 0x7448D0, 0xC0392B, 0x2563C9, 0x9A6400 },
    .icon = 0x1C1C1E, .icon_glyph = 0xF3F1EC,
    .light = true, .panel = 0xFFFFFF, .panel_edge = 0xDDDAD3, .inset = 0xF1EFEA, .inset_hi = 0xE8E5DE, .chip = 0xFFFFFF,
    .label2 = 0x5C5C61, .off = 0xB8B5AE, .sheet = 0xFFFFFF, .press_tint = 0x000000, .tile_glyph = 0xFFFFFF,
    .dcls = { 0x6A3FC4, 0xB8121C, 0x1F5FCC, 0x845400 },
    .b_ground = 0xF5F2EB, .b_tile = 0xFFFFFF, .b_border = 0xC9C1B3, .b_shadow = 0xC9C1B3, .b_lit = 0x1F4FC2, .b_lit_hi = 0x6B4A00,
    .b_unlit = 0xE2DED6, .b_tank = 0x1B6B2A, .b_face_tile = 0x101418, .b_face_text = 0xF5F2EB, .b_head = 0xF5F2EB,
    .b_tag_model = 0x8F5200, .b_tag_phys = 0x8F5200, .b_tag_learn = 0x1B6B2A, .b_text = 0x101418, .b_muted = 0x4A4A4A,
    .c_bg = 0xF3F1EC, .c_track = 0xE8E5DE, .c_rpm = 0x1A5BC4, .c_tick = 0x8C8C8C, .c_status = 0x5C5C61,
    .c_btn = 0x1C1C1E, .c_btn_glyph = 0xF3F1EC, .c_text = 0x1C1C1E, .c_muted = 0x5C5C61,
    THEME_SHAPES, THEME_FONTS,
    .face_top = 0xFFFFFF, .face_bot = 0xFFFFFF, .face_tick = 0xA34200, .face_dot = 0x8C8C8C, .face_hour = 0xA34200, .face_min = 0x1C1C1E,
    .sb_cpu = 0x0A665F, .sb_ai = 0xA34200, .sb_mips = 0x1C1C1E, .sb_pwr = 0x186A2C,
};

/* Colour (F rows, Apple-style on the OLED): wallpaper gradients, glass cards, gradient widgets, white text. The Drive
   screen keeps the black Pixel garage values. No radial glows (LV_USE_DRAW_SW_COMPLEX_GRADIENTS stays off). */
__attribute__((unused)) static const ui_theme_t THEME_COLOUR = {
    .name = "colour",
    .bg = 0x05050B, .card = 0x1A1630, .border = 0x3A3550, .text = 0xFFFFFF, .muted = 0xA3A1B5, .dim = 0x5A5870,
    .track = 0x2A2740, .good = 0x30D158, .warn = 0xFFD60A, .bad = 0xFF453A, .estimate = 0xFFB340, .ai = 0xFF453A,
    .scrim = 0x000000, .banner = 0x1A1630,
    .section = { [SEC_SKY] = 0x64D2FF, [SEC_ORANGE] = 0xFF9F0A, [SEC_GREEN] = 0x30D158, [SEC_PINK] = 0xFF6B9A,
                 [SEC_AMBER] = 0xFFB340, [SEC_RED] = 0xFF453A, [SEC_AQUA] = 0x5EEAD4, [SEC_VIOLET] = 0xBF5AF2,
                 [SEC_CYAN] = 0x5AC8FA },
    .cls = { 0xBF5AF2, 0xFF453A, 0x0A84FF, 0xFFD60A },
    .icon = 0xFFFFFF, .icon_glyph = 0xFFFFFF,
    .b_ground = 0x000000, .b_tile = 0x0B0B0D, .b_border = 0x2A2A2E, .b_shadow = 0x1A1A1D, .b_lit = 0x7AA2FF, .b_lit_hi = 0xF2C94C,
    .b_unlit = 0x222226, .b_tank = 0x7CCB6E, .b_face_tile = 0xF2ECE1, .b_face_text = 0x111111, .b_head = 0x111111,
    .b_tag_model = 0xF2A13A, .b_tag_phys = 0xF2A13A, .b_tag_learn = 0x7CCB6E, .b_text = 0xF2ECE1, .b_muted = 0xB8B8B8,
    .c_bg = 0x05050B, .c_track = 0x2A2740, .c_rpm = 0x64D2FF, .c_tick = 0x5A5870, .c_status = 0xA3A1B5,
    .c_btn = 0xFFFFFF, .c_btn_glyph = 0x05050B, .c_text = 0xFFFFFF, .c_muted = 0xA3A1B5,
    THEME_SHAPES, THEME_FONTS,
    .gradients = true, .card_opa = 23, .border_opa = 36, .glass = 0xFFFFFF,
    .wall_top = 0x120A2E, .wall_bot = 0x05050B,
    .wall_app_top = { [SEC_SKY] = 0x06121E, [SEC_ORANGE] = 0x1E0F06, [SEC_GREEN] = 0x061E0E, [SEC_PINK] = 0x1E0614,
                      [SEC_AMBER] = 0x1E1606, [SEC_RED] = 0x1E0606, [SEC_AQUA] = 0x061C1A, [SEC_VIOLET] = 0x120A2E,
                      [SEC_CYAN] = 0x06141E },
    .wall_app_bot = { [SEC_SKY] = 0x030508, [SEC_ORANGE] = 0x060403, [SEC_GREEN] = 0x030604, [SEC_PINK] = 0x060305,
                      [SEC_AMBER] = 0x060503, [SEC_RED] = 0x060303, [SEC_AQUA] = 0x030606, [SEC_VIOLET] = 0x05050B,
                      [SEC_CYAN] = 0x030508 },
    .econ_g1 = 0xFF9F0A, .econ_g2 = 0xFF375F,
    .cls_g1 = { 0xD98CFF, 0xFF6B5E, 0x5AC8FA, 0xFFB340 }, .cls_g2 = { 0x8E3BEA, 0xC81E12, 0x0A64E0, 0xFF375F },
    .end_g1 = 0xFF453A, .end_g2 = 0xD70015,
    .face_top = 0xFFFFFF, .face_bot = 0xE9E4FF, .face_tick = 0x5E5CE6, .face_dot = 0xB7B2D6, .face_hour = 0xFF375F, .face_min = 0x1C1446,
    .sb_cpu = 0x64D2FF, .sb_ai = 0xFF9F0A, .sb_mips = 0xFFFFFF, .sb_pwr = 0x30D158,
};

const ui_theme_t *ui_th = &THEME_DARK;
static uint8_t s_accent_sec = SEC_SKY;
uint32_t ui_accent(void) { return UI_THEME.section[s_accent_sec]; }

/* ---------- state ---------- */
static bool s_light;                                   /* the launcher look: light (Settings > Theme) */
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;
static lv_obj_t *s_home, *s_appscr, *s_body;
static int s_cur = -1;                         /* app index on screen, -1 = home */
static bool s_panel_on;
/* Screen off = brightness 0 + display off, WITHOUT the CO5300 sleep-in: measured 2026-11-11, every sleep-out (4 of 4)
   left the CST9220 touch controller answering NACK / 0xff reports from ~4.8 s after the wake (no touch until it was
   re-opened); with display off only, 20 of 20 wakes had no bad read. */
#define s_panel_slpin false
static volatile bool s_req_home, s_req_off, s_req_wake;
static volatile int s_req_bright = -1;
static uint32_t s_bright_cmds;                         /* brightness commands sent to the panel (LVGL task) */
static ui_settings_t s_set;
static portMUX_TYPE s_set_mux = portMUX_INITIALIZER_UNLOCKED;

EXT_RAM_BSS_ATTR static app_status_t s_st;
EXT_RAM_BSS_ATTR static ui_snap_t s_snap;
static volatile bool s_producers_ready;
void mosaico_ui_producers_ready(void) { s_producers_ready = true; mosaico_drive_start(); }
static int64_t s_last_snap_ms = -100000, s_last_app_ms, s_last_sec_ms;

/* render time of each LVGL refresh (all dirty areas): the worst one in each 10 s window is logged with the theme */
static int64_t s_rs_us;
static uint32_t s_tsnap_us __attribute__((unused));                              /* snapshot sliding: the last page picture's render time */
static uint32_t s_render_max_us;
#if CONFIG_OCM_SWIPE_INTENT
/* smart swipe (swipe_intent.h, the block before the boot splash): Home's content generation and the Home picture's copy of
   it, so a picture counts as current only if nothing on Home changed since it was taken; s_si_act = the learner acts on
   the gesture in flight (arm A of the A/B test) */
static uint32_t s_ui_mut, s_home_gen, s_home_pic_gen = UINT32_MAX;
static bool s_si_act;
static void si_touch(lv_indev_t *indev, bool pressed, int32_t x, int32_t y);
static void si_attach(lv_obj_t *tv);
static void si_warm_tick(void);
static void si_render_ready(void);
#define SI_MUT() (s_ui_mut++)
#define SI_HOME_PIC_CURRENT() (s_si_act && s_home_pic_gen == s_home_gen)
#else
#define SI_MUT() ((void)0)
#define SI_HOME_PIC_CURRENT() false
#endif
#if CONFIG_OCM_UI_METRICS
/* Smart UI metrics (ui_metrics.h, the block before the boot splash). The settle detector's two flag writes in the display
   event callbacks are the only per-frame work; everything else runs once per touch event. */
#include "ui_metrics.h"
static um_settle_t s_ums;
static void um_touch(lv_indev_t *indev, bool pressed);
static void um_frame(void);
static void um_indev_cb(lv_event_t *e);
static void um_boot(void);
/* LVGL itself sends INVALIDATE_AREA while it renders (get_max_row probes the rounder with a dummy area), and a real
   invalidation is not allowed then (lv_refr.c asserts it): only one outside a render means an area waits */
static bool s_um_rendering;
#define UM_INVALIDATED()  do { if (!s_um_rendering) s_ums.dirty = 1; } while (0)
#define UM_RENDER_START() (s_ums.dirty = 0, s_um_rendering = true)
#define UM_FRAME() do { s_um_rendering = false; if (s_ums.phase == UM_S_WAIT) um_frame(); } while (0)
#define UM_MAX_ICONS (UI_MAX_APPS)
static lv_obj_t *s_icon_cell[UM_MAX_ICONS];            /* the launcher's app cells, for "which app did the press hit" */
static uint8_t s_icon_app[UM_MAX_ICONS], s_nicons;
#else
#define UM_INVALIDATED()  ((void)0)
#define UM_RENDER_START() ((void)0)
#define UM_FRAME() ((void)0)
#endif
#if CONFIG_OCM_UI_PREFETCH
/* Smart UI arm A1, press-time prefetch (ui_prefetch.h; the block after the metrics block) */
#include "ui_prefetch.h"
static upf_t s_pf;
static lv_obj_t *s_pf_scr, *s_pf_body;                 /* the spare app screen: at most one app is prebuilt in it */
static int s_pf_app = -1, s_pd_app = -1;               /* the prebuilt app; the app whose teardown waits for the settle */
static bool s_pf_home;                                 /* Back: the launcher behind the app was refreshed at the press */
static void pf_act(int action);
static void pd_run(void);
static void pf_cell_pressing_cb(lv_event_t *e);
static bool pf_open_app(int idx);
static bool pf_go_home(void);
#endif
/* the UI job's own soft-deadline figures */
static sched_job_t s_job = -1;
static uint32_t s_runs, s_misses, s_max_us, s_late_max_ms;
#define UI_PERIOD_MS 50

/* ---------- helpers for apps ---------- */
void ui_set_text(lv_obj_t *o, const char *t)
{
    if (!o) return;
    const char *cur = lv_label_get_text(o);
    if (t && cur && strcmp(cur, t) == 0) return;          /* LVGL redraws on every set: only on change */
    SI_MUT();
    lv_label_set_text(o, t);
}

void ui_set_fmt(lv_obj_t *o, const char *fmt, ...)
{
    EXT_RAM_BSS_ATTR static char buf[256];                /* LVGL task only */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_set_text(o, buf);
}

void ui_set_color(lv_obj_t *o, uint32_t c)
{
    if (!o) return;
    lv_color_t want = lv_color_hex(c);
    if (lv_color_to_u32(lv_obj_get_style_text_color(o, 0)) == lv_color_to_u32(want)) return;
    SI_MUT();
    lv_obj_set_style_text_color(o, want, 0);
}

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t border)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, UI_THEME.radius_card, 0);
    lv_obj_set_style_border_width(c, border == UI_BORDER ? UI_THEME.border_card : UI_THEME.border_accent, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(border), 0);
    lv_obj_set_style_pad_all(c, UI_THEME.pad_card, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);         /* presses go to the screen: swipes work everywhere */
    if (border == UI_BORDER) ui_glass(c);
    else if (UI_THEME.gradients) { ui_glass(c); lv_obj_set_style_border_color(c, lv_color_hex(border), 0); lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0); }
    return c;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_caption(lv_obj_t *parent, const char *upper_text)
{
    lv_obj_t *l = ui_label(parent, UI_THEME.f_small, UI_MUTED, upper_text);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

lv_obj_t *ui_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void ui_glass(lv_obj_t *o)
{
    if (!UI_THEME.gradients) return;
    lv_obj_set_style_bg_color(o, lv_color_hex(UI_THEME.glass), 0);
    lv_obj_set_style_bg_opa(o, UI_THEME.card_opa, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(UI_THEME.glass), 0);
    lv_obj_set_style_border_opa(o, UI_THEME.border_opa, 0);
    if (lv_obj_get_style_border_width(o, 0) == 0) lv_obj_set_style_border_width(o, 1, 0);
}

void ui_grad(lv_obj_t *o, uint32_t top, uint32_t bottom)
{
    if (!UI_THEME.gradients) return;
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
}

void ui_wallpaper(lv_obj_t *o, int section)
{
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(UI_BG), 0);
    if (!UI_THEME.gradients) return;
    if (section < 0) ui_grad(o, UI_THEME.wall_top, UI_THEME.wall_bot);
    else ui_grad(o, UI_THEME.wall_app_top[section], UI_THEME.wall_app_bot[section]);
}

void ui_set_bg(lv_obj_t *o, uint32_t c)
{
    if (!o) return;
    if (lv_color_to_u32(lv_obj_get_style_bg_color(o, 0)) != lv_color_to_u32(lv_color_hex(c))) lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
}

lv_obj_t *ui_pill(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *l = ui_label(parent, UI_THEME.f_small, color, text);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(l, 8, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
    return l;
}

static void back_cb(lv_event_t *e) { (void)e; ui_go_home(); }

lv_obj_t *ui_header(lv_obj_t *parent, const char *title)
{
    lv_obj_t *b = lv_obj_create(parent);                  /* round back chevron */
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 40, 40);
    lv_obj_set_pos(b, UI_MARGIN + 8, UI_TOP);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_ext_click_area(b, 12);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(b, UI_FLAG_BACK);
    lv_obj_center(ui_label(b, UI_THEME.f_body, UI_ORANGE, LV_SYMBOL_LEFT));
    lv_obj_t *t = ui_label(parent, UI_THEME.f_title, UI_TEXT, title);
    lv_obj_set_pos(t, UI_MARGIN + 60, UI_TOP + 6);
    return t;
}

const char *ui_class_name(int k)
{
    static const char *const C[4] = {"Stopped", "Braking", "Accelerating", "Cruising"};
    return k >= 0 && k < 4 ? C[k] : "--";
}

void ui_mark_jobs(uint32_t *runs, uint32_t *misses, uint32_t *max_us, uint32_t *late_max_ms)
{
    *runs = s_runs; *misses = s_misses; *max_us = s_max_us; *late_max_ms = s_late_max_ms;
}

const char *ui_econ(float kpl, char *buf, size_t n)
{
    ui_settings_t set;
    ui_settings_get(&set);
    if (kpl < 0.05f) snprintf(buf, n, "--");
    else if (set.econ_l100) snprintf(buf, n, "%.1f", 100.0 / kpl);
    else snprintf(buf, n, "%.1f", (double)kpl);
    return set.econ_l100 ? "L/100 km" : "km/L";
}

/* ---------- confirm: a bottom sheet over a scrim; only its red button acts ---------- */
static lv_obj_t *s_modal;
static void (*s_modal_cb)(void *);
static void *s_modal_arg;
static bool s_modal_open(void) { return s_modal != NULL; }

static void modal_close(void *arg) { (void)arg; if (s_modal) { lv_obj_delete(s_modal); s_modal = NULL; } }
static void modal_no(lv_event_t *e) { (void)e; lv_async_call(modal_close, NULL); }
/* after the yes button's own event has finished: close, then act (the action may open the next question) */
static void modal_run(void *unused)
{
    (void)unused;
    void (*cb)(void *) = s_modal_cb;
    void *arg = s_modal_arg;
    s_modal_cb = NULL;
    modal_close(NULL);
    if (cb) cb(arg);
}
static void modal_yes(lv_event_t *e) { (void)e; lv_async_call(modal_run, NULL); }

static lv_obj_t *modal_button(lv_obj_t *p, const char *t, uint32_t bg, uint32_t fg, lv_event_cb_t cb, lv_align_t al)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, 196, 54);
    lv_obj_set_ext_click_area(b, 5);                      /* a 64 px tall touch target on the 54 px button */
    lv_obj_align(b, al, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(b, UI_THEME.f_body, fg, t));
    return b;
}

void ui_confirm(const char *text, const char *yes, void (*cb)(void *arg), void *arg)
{
    if (s_modal) { lv_obj_delete_async(s_modal); s_modal = NULL; }
    s_modal_cb = cb; s_modal_arg = arg;
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(UI_THEME.scrim), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_60, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);              /* swallows taps behind it */
    lv_obj_t *c = ui_card(s_modal, UI_MARGIN, UI_H - 220 - UI_MARGIN, UI_W - 2 * UI_MARGIN, 220, UI_BORDER);
    lv_obj_t *t = ui_label(c, UI_THEME.f_value, UI_TEXT, text);
    lv_obj_set_width(t, UI_W - 2 * UI_MARGIN - 2 * UI_THEME.pad_card - 4);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(t, 2, 2);
    modal_button(c, "Cancel", UI_THEME.track, UI_TEXT, modal_no, LV_ALIGN_BOTTOM_LEFT);
    modal_button(c, yes, UI_RED, UI_BG, modal_yes, LV_ALIGN_BOTTOM_RIGHT);   /* white on the dark red was 4.0:1 */
}

/* ---------- End drive / Start drive control: a 64 px ring with a stop icon, held 1 s (the ring fills) to end a drive,
   so a brush of the glass never ends one; after an End drive it offers Start drive (a tap) until the car moves ---------- */
enum { CTL_HIDDEN = 0, CTL_END, CTL_START, CTL_BUSY };
typedef struct { lv_obj_t *arc, *icon, *label, *fill, *pill; uint32_t t0; bool fired; uint32_t red; } ctl_t;

/* hold progress 0..100: the ring fills, or the pill fills from the left */
static void ctl_progress(ctl_t *k, int v)
{
    if (k->arc) lv_arc_set_value(k->arc, v);
    if (k->fill) lv_obj_set_width(k->fill, (int32_t)(v * (lv_obj_get_width(k->pill) - 4) / 100));
}

static void ctl_event(lv_event_t *e)
{
    lv_obj_t *c = lv_event_get_current_target_obj(e);
    ctl_t *k = (ctl_t *)lv_event_get_user_data(e);
    int mode = (int)(intptr_t)lv_obj_get_user_data(c);
    lv_event_code_t code = lv_event_get_code(e);
    if (mode == CTL_START) {
        if (code == LV_EVENT_CLICKED) { mosaico_drive_start_request(); mosaico_board_request(MB_BUZZ, 0); }
        return;
    }
    if (mode != CTL_END) return;
    if (code == LV_EVENT_PRESSED) { k->t0 = lv_tick_get(); k->fired = false; }
    else if (code == LV_EVENT_PRESSING && !k->fired) {
        uint32_t ms = lv_tick_elaps(k->t0);
        ctl_progress(k, (int)(ms >= 1000 ? 100 : ms / 10));
        if (ms >= 1000) {
            k->fired = true;
            mosaico_drive_end_request();
            mosaico_board_request(MB_BUZZ, 0);
            ESP_LOGI(TAG, "End drive held 1 s");
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!k->fired) ctl_progress(k, 0);
    }
}

static void ctl_free(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

lv_obj_t *ui_drive_ctl(lv_obj_t *parent, int cx, int y)
{
    ctl_t *k = lv_malloc(sizeof *k);
    memset(k, 0, sizeof *k);
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 200, 64);
    lv_obj_set_pos(c, cx - 100, y);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    k->arc = lv_arc_create(c);
    lv_obj_set_size(k->arc, 64, 64);
    lv_obj_set_pos(k->arc, 0, 0);
    lv_arc_set_rotation(k->arc, 270);
    lv_arc_set_bg_angles(k->arc, 0, 360);
    lv_arc_set_range(k->arc, 0, 100);
    lv_arc_set_value(k->arc, 0);
    lv_obj_remove_style(k->arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(k->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(k->arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(k->arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(k->arc, lv_color_hex(UI_RED), LV_PART_MAIN);
    lv_obj_set_style_arc_color(k->arc, lv_color_hex(UI_RED), LV_PART_INDICATOR);
    k->icon = ui_label(k->arc, UI_THEME.f_value, UI_RED, LV_SYMBOL_STOP);
    lv_obj_center(k->icon);
    k->label = ui_label(c, UI_THEME.f_body, UI_TEXT, "End drive");
    lv_obj_align(k->label, LV_ALIGN_LEFT_MID, 76, 0);
    lv_obj_add_event_cb(c, ctl_event, LV_EVENT_ALL, k);
    lv_obj_add_event_cb(c, ctl_free, LV_EVENT_DELETE, k);
    lv_obj_set_user_data(c, (void *)(intptr_t)CTL_HIDDEN);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    /* the ctl_t is reachable from the arc's parent for refresh */
    lv_obj_set_user_data(k->arc, k);
    return c;
}

/* E_Home: "Hold: End drive" pill, 44 px high, red border, a red square; the pill fills while held */
lv_obj_t *ui_drive_pill(lv_obj_t *parent, int x, int y, int w, int h)
{
    ctl_t *k = lv_malloc(sizeof *k);
    memset(k, 0, sizeof *k);
    k->red = s_light && parent != NULL && lv_obj_get_screen(parent) == s_home ? 0xC4141E : UI_THEME.section[SEC_RED];
    lv_obj_t *c = k->pill = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(UI_THEME.section[SEC_RED]), 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (UI_THEME.gradients) { ui_grad(c, UI_THEME.end_g1, UI_THEME.end_g2); lv_obj_set_style_border_width(c, 0, 0); }
    k->fill = ui_rect(c, 2, 2, 0, h - 4, UI_THEME.gradients ? 0xFFFFFF : UI_THEME.section[SEC_RED], LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(k->fill, LV_OPA_40, 0);
    k->icon = ui_rect(c, 16, h / 2 - 5, 10, 10, UI_THEME.gradients ? 0xFFFFFF : UI_THEME.section[SEC_RED], 2);
    k->label = ui_label(c, UI_THEME.f_label, UI_THEME.gradients ? 0xFFFFFF : UI_THEME.section[SEC_RED], "Hold: End drive");
    lv_obj_align(k->label, LV_ALIGN_LEFT_MID, 34, 0);
    lv_obj_add_event_cb(c, ctl_event, LV_EVENT_ALL, k);
    lv_obj_add_event_cb(c, ctl_free, LV_EVENT_DELETE, k);
    lv_obj_set_user_data(c, (void *)(intptr_t)CTL_HIDDEN);
    lv_obj_set_user_data(k->fill, k);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

void ui_drive_ctl_refresh(lv_obj_t *c, const ui_snap_t *s)
{
    if (!c) return;
    lv_obj_t *first = lv_obj_get_child(c, 0);
    ctl_t *k = (ctl_t *)lv_obj_get_user_data(first);
    int mode = mosaico_drive_ending() ? CTL_BUSY : s->drive_active ? CTL_END : CTL_HIDDEN;
    int was = (int)(intptr_t)lv_obj_get_user_data(c);
    if (mode == was) return;
    lv_obj_set_user_data(c, (void *)(intptr_t)mode);
    if (mode == CTL_HIDDEN) { lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN);
    uint32_t col = mode == CTL_START ? UI_GREEN : (k->red ? k->red : UI_THEME.section[SEC_RED]);
    if (k->arc) {
        lv_obj_set_style_arc_color(k->arc, lv_color_hex(col), LV_PART_MAIN);
        lv_obj_set_style_arc_color(k->arc, lv_color_hex(col), LV_PART_INDICATOR);
        ui_set_color(k->icon, col);
        ui_set_text(k->icon, mode == CTL_START ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP);
        ui_set_text(k->label, mode == CTL_END ? "End drive\n(hold 1 s)" : mode == CTL_START ? "Start drive" : "Saving...");
    } else if (UI_THEME.gradients) {
        ui_grad(c, mode == CTL_START ? 0x4CE27A : UI_THEME.end_g1, mode == CTL_START ? 0x16A34A : UI_THEME.end_g2);
        ui_set_bg(k->icon, 0xFFFFFF);
        lv_obj_set_style_radius(k->icon, mode == CTL_START ? LV_RADIUS_CIRCLE : 2, 0);
        ui_set_color(k->label, 0xFFFFFF);
        ui_set_text(k->label, mode == CTL_END ? "Hold: End drive" : mode == CTL_START ? "Start drive" : "Saving...");
    } else {
        lv_obj_set_style_border_color(c, lv_color_hex(col), 0);
        ui_set_bg(k->icon, col);
        ui_set_bg(k->fill, col);
        lv_obj_set_style_radius(k->icon, mode == CTL_START ? LV_RADIUS_CIRCLE : 2, 0);
        ui_set_color(k->label, col);
        ui_set_text(k->label, mode == CTL_END ? "Hold: End drive" : mode == CTL_START ? "Start drive" : "Saving...");
    }
    ctl_progress(k, mode == CTL_BUSY ? 100 : 0);
}

/* ---------- settings ---------- */
const char *ui_widget_name(int w)
{
    static const char *const N[W_COUNT] = {"FUEL RATE", "FUEL USE NOW", "ECONOMY THIS DRIVE", "SPEED", "ENGINE SPEED", "TORQUE"};
    return w >= 0 && w < W_COUNT ? N[w] : "";
}

void ui_settings_get(ui_settings_t *out)
{
    portENTER_CRITICAL(&s_set_mux);
    *out = s_set;
    portEXIT_CRITICAL(&s_set_mux);
}

static volatile bool s_theme_check;
/* deferred launcher work after a Settings change (LVGL task only): one theme check once the theme taps stop, one
   launcher rebuild once the Home order moves stop. A tap itself only restyles the tapped control. */
static ocm_debounce_t s_theme_deb, s_launch_deb;

void ui_settings_set(const ui_settings_t *in)
{
    ui_settings_t old;
    portENTER_CRITICAL(&s_set_mux);
    old = s_set;
    s_set = *in;
    portEXIT_CRITICAL(&s_set_mux);
    unsigned act = ocm_settings_actions(ocm_settings_diff(&old, in));
    int64_t now = esp_timer_get_time() / 1000;
    if (act & OCM_DO_SAVE) mosaico_settings_save_request(in, sizeof *in);   /* the board task writes it, debounced */
    if (act & OCM_DO_PANEL) s_req_bright = in->brightness;
    if (act & OCM_DO_THEME) ocm_debounce_touch(&s_theme_deb, now, OCM_THEME_QUIET_MS);
    if (act & OCM_DO_LAUNCHER) ocm_debounce_touch(&s_launch_deb, now, OCM_LAUNCHER_QUIET_MS);
}

/* Settings brightness slider: the preview is applied by the next UI tick, so the panel gets at most one command per
   50 ms however fast the finger moves; the value is saved (one NVS write) only by ui_settings_set_brightness on release */
void ui_brightness_preview(int pct)
{
    s_req_bright = pct < 10 ? 10 : pct > 100 ? 100 : pct;
}

void ui_settings_set_brightness(int pct)
{
    ui_settings_t s;
    portENTER_CRITICAL(&s_set_mux);
    s_set.brightness = (uint8_t)(pct < 10 ? 10 : pct > 100 ? 100 : pct);
    s = s_set;
    portEXIT_CRITICAL(&s_set_mux);
    mosaico_settings_save_request(&s, sizeof s);
    s_req_bright = s.brightness;
    static uint32_t cmds0;
    ESP_LOGI(TAG, "brightness %u %% (saved; %lu panel commands since the last save)", s.brightness, (unsigned long)(s_bright_cmds - cmds0));
    cmds0 = s_bright_cmds;
}

static void settings_defaults(ui_settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->ver = UI_SETTINGS_VER;
    /* the launcher order is the registry order (four a page as on H_Apps) */
    s->n = (uint8_t)ui_app_count();
    for (int i = 0; i < s->n; i++) s->order[i] = (uint8_t)i;
    s->widget = W_KPL;
    s->brightness = 60;
    s->screen_off_s = 0;                                 /* a car dashboard: on until set otherwise */
    s->theme = 0;                                        /* Auto: Light 06:00-17:00 once the clock is known, Dark otherwise */
}

static bool s_set_loaded;                              /* false: defaults (a first boot or a settings reset) */
static void settings_load(void)
{
    ui_settings_t s, m;
    settings_defaults(&s_set);
    if (!mosaico_settings_load(&s, sizeof s)) return;
    int wifi = -1;
    for (int i = 0; i < ui_app_count(); i++) if (!strcmp(ui_app(i)->id, "wifi")) wifi = i;
    /* version 5 -> 6: the Wi-Fi app goes in before Settings and every hidden app shows again (read once, then saved);
       invalid indices are dropped and apps added in a newer firmware are appended */
    int r = ocm_settings_migrate(&s, &m, ui_app_count(), wifi);
    if (r == OCM_SET_REJECT) return;
    if (m.widget >= W_COUNT) m.widget = W_KPL;
    if (m.theme > 2) m.theme = 0;
    if (m.brightness < 5 || m.brightness > 100) m.brightness = 60;
    s_set = m;
    s_set_loaded = true;
    if (r == OCM_SET_CHANGED) {
        int hidden = 0;
        for (int i = 0; i < 16; i++) hidden += (s.hidden_v5 >> i) & 1;
        ESP_LOGI(TAG, "UI settings: version %u -> %u, %d hidden app(s) shown again, order kept", s.ver, m.ver, hidden);
        mosaico_settings_save_request(&s_set, sizeof s_set);
    }
}

/* ---------- snapshot: one copy of every producer's published state ---------- */
static float s_pw_hist[60];
static int s_pw_n;                                       /* watts, one a second while the gauge measures a discharge */
#define UI_SIM_BATT false

static void take_snapshot(void)
{
    ui_snap_t *s = &s_snap;
    status_get(&s_st);
    s->st = &s_st;
    ml_get_stats(&s->ml);
    s->model = ml_model_active();
    s->nprobs = ml_get_last_probs(s->probs, 4);
    if (s_producers_ready) rev_get_status(&s->rs);       /* the R.E.V learner exists only after rev_init() */
    else { memset(&s->rs, 0, sizeof s->rs); s->rs.cal = 1.0f; }
    mosaico_gauge_read(&s->gauge);
    mosaico_storage_read(&s->fat);
    s->now_ms = esp_timer_get_time() / 1000;
    s->obd_live = s_st.ble == BLE_ST_LOGGING && (OBD_HAS(&s_st.obd, F_rpm) || OBD_HAS(&s_st.obd, F_speed));
    s->drive_active = elm_drive_active();
    mosaico_clock_poll();
    if (mosaico_clock_known(&s_st)) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(s->clock, sizeof s->clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        int m = (int)(s->now_ms / 60000);
        if (m < 60) snprintf(s->clock, sizeof s->clock, "up %dm", m);
        else snprintf(s->clock, sizeof s->clock, "up %dh", m / 60);
    }
    s_last_snap_ms = s->now_ms;
}

/* ---------- the launcher look: dark, or light (accents that pass 4.5:1 on the light wallpaper) ---------- */
#define HT       (s_light ? 0x1C1C1E : UI_TEXT)
#define HM       (s_light ? 0x6E6E73 : UI_MUTED)
#define HD       (s_light ? 0xC6C4BF : UI_THEME.dim)
#define HTRACK   (s_light ? 0xE3E1DC : 0x262629)
#define HC_CPU   (s_light ? 0x0B7F79 : UI_THEME.sb_cpu)
#define HC_C0    (s_light ? 0x1F5FCC : 0x8CC8FF)
#define HC_C1    (s_light ? 0xB54A00 : 0xFE860F)
#define HC_PW    (s_light ? 0x9A5B00 : UI_THEME.section[SEC_AMBER])
#define HC_SKY   (s_light ? 0x1F5FCC : UI_THEME.section[SEC_SKY])
#define HC_RED   (s_light ? 0xC4141E : UI_THEME.section[SEC_RED])
#define HC_WARN  (s_light ? 0x9A6A00 : UI_THEME.warn)
#define HC_GREEN (s_light ? 0x1E7B34 : UI_GREEN)

/* ---------- glass for the wallpaper screens (Home, Apps): dark = white at opa 26, border opa 18; light = white at opa
   184, border opa 242 (the design's soft shadow is left out) ---------- */
static void h_glass(lv_obj_t *o, int radius)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(o, s_light ? 184 : 26, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(o, s_light ? 242 : 18, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, radius, 0);
}

/* a plain flex container (no style, not clickable, not scrollable) */
static lv_obj_t *flex_box(lv_obj_t *p, lv_flex_flow_t flow, lv_flex_align_t main, lv_flex_align_t cross, int gap)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_flow(b, flow);
    lv_obj_set_flex_align(b, main, cross, cross);
    lv_obj_set_style_pad_gap(b, gap, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return b;
}
static lv_obj_t *grow(lv_obj_t *p) { lv_obj_t *g = flex_box(p, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 0);
                                     lv_obj_set_height(g, 1); lv_obj_set_flex_grow(g, 1); return g; }

/* ---------- status bar (H): a flex column on lv_layer_top. Row 1 (flex row): the 28 px OpenCarMind mark, the time, a
   spacer, on Drive the CPU and power figures, BT, Wi-Fi, battery icon and %. Row 2: the counter pill (hidden on Home and
   on Drive, where the SYSTEM card and row 1 show the same numbers). Nothing is placed by pixel offsets next to text. ---------- */
extern const lv_image_dsc_t ocm_logo_28, ocm_logo_160;
static lv_obj_t *s_sb, *sb_row1, *sb_time, *sb_dcpu, *sb_dpwr, *sb_bt, *sb_wifi, *sb_batt_i, *sb_batt, *sb_pill, *sb_grp[4], *sb_val[4],
                *s_banner, *s_banner_txt;
enum { SB_HOME = 0, SB_DRIVE, SB_APP };
static int s_sb_mode = -1;

static void build_status_bar(void)
{
    lv_obj_t *top = lv_layer_top();
    s_sb = flex_box(top, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 6);
    lv_obj_set_pos(s_sb, 22, 12);
    lv_obj_set_size(s_sb, UI_W - 44, LV_SIZE_CONTENT);
    sb_row1 = flex_box(s_sb, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 10);
    lv_obj_set_size(sb_row1, LV_PCT(100), 32);
    lv_obj_set_style_pad_left(sb_row1, 10, 0);
    lv_obj_set_style_pad_right(sb_row1, 22, 0);             /* the panel's rounded corner */
    lv_obj_t *logo = lv_image_create(sb_row1);
    lv_image_set_src(logo, &ocm_logo_28);
    sb_time = ui_label(sb_row1, UI_THEME.f_title, UI_TEXT, "--:--");
    grow(sb_row1);
    sb_dcpu = ui_label(sb_row1, UI_THEME.f_small, UI_THEME.sb_cpu, "");
    sb_dpwr = ui_label(sb_row1, UI_THEME.f_small, UI_THEME.section[SEC_AMBER], "");
    sb_bt = ui_label(sb_row1, UI_THEME.f_body, UI_MUTED, LV_SYMBOL_BLUETOOTH);
    sb_wifi = ui_label(sb_row1, UI_THEME.f_body, UI_MUTED, LV_SYMBOL_WIFI);
    lv_obj_t *bat = flex_box(sb_row1, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 6);
    lv_obj_set_size(bat, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    sb_batt_i = ui_label(bat, UI_THEME.f_body, UI_GREEN, LV_SYMBOL_BATTERY_FULL);
    sb_batt = ui_label(bat, UI_THEME.f_body, UI_TEXT, "");
    sb_pill = flex_box(s_sb, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_size(sb_pill, LV_PCT(100), 26);
    lv_obj_set_style_radius(sb_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sb_pill, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_opa(sb_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sb_pill, 1, 0);
    lv_obj_set_style_border_color(sb_pill, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_pad_hor(sb_pill, 14, 0);
    static const char *const L[4] = {"CPU", "AI", "MIPS", "POWER"};
    const uint32_t C[4] = {UI_THEME.sb_cpu, UI_THEME.sb_ai, UI_THEME.sb_mips, UI_THEME.sb_pwr};
    for (int k = 0; k < 4; k++) {
        lv_obj_t *g = sb_grp[k] = flex_box(sb_pill, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 6);
        lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_t *l = ui_label(g, UI_THEME.f_small, UI_MUTED, L[k]);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        sb_val[k] = ui_label(g, UI_THEME.f_body, C[k], "--");
    }

    s_banner = lv_obj_create(top);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, 400, LV_SIZE_CONTENT);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(UI_THEME.banner), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_banner, UI_THEME.radius_row, 0);
    lv_obj_set_style_border_width(s_banner, UI_THEME.border_accent, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(UI_THEME.warn), 0);
    lv_obj_set_style_pad_all(s_banner, 12, 0);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);
    s_banner_txt = ui_label(s_banner, UI_THEME.f_body, UI_TEXT, "");
    lv_obj_set_width(s_banner_txt, 372);
    lv_label_set_long_mode(s_banner_txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_banner_txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    s_sb_mode = -1;
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) { SI_MUT(); if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
}

static void power_text(const ui_snap_t *s, char *b, size_t n, bool *measured)
{
    *measured = false;
    if (!s->gauge.ok) snprintf(b, n, "--");
    else if (s->gauge.ma > -5 && s->gauge.ma < 5) snprintf(b, n, "USB");      /* USB current is not measured */
    else { snprintf(b, n, "%.2f W", s->gauge.mv * fabs((double)s->gauge.ma) / 1e6); *measured = true; }
}

static void batt_text(const ui_snap_t *s, char *b, size_t n)
{
    if (!s->gauge.ok) { snprintf(b, n, "--"); return; }
    /* the BQ27220 still has its 3000 mAh design capacity (the cell is 65 mAh), so its % is meaningless: volts until it is set */
    if (mosaico_gauge_uncalibrated(&s->gauge)) snprintf(b, n, "%.2f V", s->gauge.mv / 1000.0);
    else snprintf(b, n, "%d%%", s->gauge.soc);
}

static void refresh_status_bar(const ui_snap_t *s)
{
    const app_status_t *st = s->st;
    int mode = s_cur < 0 ? SB_HOME : !strcmp(ui_app(s_cur)->id, "drive") ? SB_DRIVE : SB_APP;
    if (mode != s_sb_mode) {
        s_sb_mode = mode;
        bool lt = s_light && mode == SB_HOME;
        ui_set_color(sb_time, lt ? 0x1C1C1E : UI_TEXT);
        ui_set_color(sb_batt, lt ? 0x1C1C1E : UI_TEXT);
        show(sb_pill, mode == SB_APP);
        show(sb_dcpu, mode == SB_DRIVE);
        show(sb_dpwr, mode == SB_DRIVE);
    }
    ui_set_text(sb_time, s->clock);
    bool lt = s_light && mode == SB_HOME;
    ui_set_color(sb_bt, st->ble == BLE_ST_LOGGING ? (s->obd_live ? (lt ? 0x1E7B34 : UI_GREEN) : (lt ? 0x9A6A00 : UI_WARN))
                        : st->ble == BLE_ST_LOCKED ? (lt ? 0xC4141E : UI_RED) : st->ble == BLE_ST_IDLE ? (lt ? 0x6E6E73 : UI_MUTED) : (lt ? 0x9A6A00 : UI_WARN));
    ui_set_color(sb_wifi, st->wifi == WIFI_ST_CONNECTED ? (lt ? 0x1E7B34 : UI_GREEN) : st->wifi == WIFI_ST_DISABLED ? (lt ? 0xAEAEB2 : UI_THEME.dim)
                          : st->wifi == WIFI_ST_IDLE ? (lt ? 0x6E6E73 : UI_MUTED) : (lt ? 0x9A6A00 : UI_WARN));
    char b[24];
    batt_text(s, b, sizeof b);
    ui_set_text(sb_batt, b);
    int soc = s->gauge.ok ? s->gauge.soc : 0;
    ui_set_text(sb_batt_i, !s->gauge.ok ? LV_SYMBOL_BATTERY_EMPTY : s->gauge.ma > 5 ? LV_SYMBOL_CHARGE : soc > 80 ? LV_SYMBOL_BATTERY_FULL
                           : soc > 55 ? LV_SYMBOL_BATTERY_3 : soc > 30 ? LV_SYMBOL_BATTERY_2 : soc > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY);
    ui_set_color(sb_batt_i, !s->gauge.ok ? (lt ? 0x6E6E73 : UI_MUTED) : (lt ? 0x1E7B34 : UI_GREEN));
    int cpu = (st->cpu_pct[0] + st->cpu_pct[1] + 1) / 2;
    bool meas;
    power_text(s, b, sizeof b, &meas);
    if (mode == SB_DRIVE) { ui_set_fmt(sb_dcpu, "CPU %d%%", cpu); ui_set_text(sb_dpwr, b); }
    if (mode == SB_APP) {
        ui_set_fmt(sb_val[0], "%d%%", cpu);
        ui_set_fmt(sb_val[1], "%d%%", st->ai_pct);
        show(sb_grp[2], st->mips_ok);                   /* no number under MIPS unless minstret passed */
        if (st->mips_ok) ui_set_fmt(sb_val[2], "%.0f", (double)(st->pmu_mips[0] + st->pmu_mips[1]));
        ui_set_text(sb_val[3], b);
    }

    const char *txt = NULL;
    uint32_t col = UI_WARN;
    /* floating banner: critical alarms, and the desktop pairing code (it must be read off the screen); prompts such as
       "No engine data yet" go into the Home smart card instead */
    if (st->alarm_text[0] && st->alarm_critical) { txt = st->alarm_text; col = UI_RED; }
    else if (st->sys_popup[0] && s->now_ms < st->sys_popup_until_ms && !strncmp(st->sys_popup, "Desktop pairing code", 20)) { txt = st->sys_popup; col = UI_GREEN; }
    if (txt) {
        ui_set_text(s_banner_txt, txt);
        if (lv_color_to_u32(lv_obj_get_style_border_color(s_banner, 0)) != lv_color_to_u32(lv_color_hex(col)))
            lv_obj_set_style_border_color(s_banner, lv_color_hex(col), 0);
    }
    show(s_banner, txt != NULL);
}

/* ---------- wallpaper (Home and Apps): computed once at boot into PSRAM by a low-priority task. Vertical lerp #121318 to
   #08080A, two soft glows alpha = a exp(-((x-cx)^2+(y-cy)^2)/(2 s^2)) (#F2A13A a 0.16 at (38,0) s 150; #72ECE4 a 0.12 at
   (480,480) s 165), composited out = out (1-a) + c a, then a 4x4 Bayer dither of +-0.5 LSB before rounding to 5/6/5 bits so
   the dark gradient does not band. Plain black until it is ready. ---------- */
static volatile bool s_wall_ready;
static lv_obj_t *s_wall_img;
static uint32_t s_wall_ms;

typedef struct { float cx, cy, sg, a; uint8_t r, g, b; } glow_t;

/* one wallpaper: vertical lerp top -> bottom, glows composited in order, Bayer 4x4 dither, RGB565 */
static uint16_t *wall_make(const uint8_t top[3], const uint8_t bot[3], const glow_t *gl, int ng)
{
    uint16_t *px = heap_caps_malloc(UI_W * UI_H * 2, MALLOC_CAP_SPIRAM);
    if (!px) return NULL;
    static const uint8_t B4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    EXT_RAM_BSS_ATTR static float ex[4][UI_W], ey[4][UI_H];        /* the Gaussian is separable: exp(x term) x exp(y term) */
    for (int k = 0; k < ng; k++) {
        for (int i = 0; i < UI_W; i++) { float d = (i - gl[k].cx) / gl[k].sg; ex[k][i] = expf(-0.5f * d * d); }
        for (int j = 0; j < UI_H; j++) { float d = (j - gl[k].cy) / gl[k].sg; ey[k][j] = expf(-0.5f * d * d); }
    }
    for (int y = 0; y < UI_H; y++) {
        float t = y / (float)(UI_H - 1);
        float base[3] = {top[0] + (bot[0] - top[0]) * t, top[1] + (bot[1] - top[1]) * t, top[2] + (bot[2] - top[2]) * t};
        for (int x = 0; x < UI_W; x++) {
            float c[3] = {base[0], base[1], base[2]};
            for (int k = 0; k < ng; k++) {
                float a = gl[k].a * ex[k][x] * ey[k][y];
                c[0] = c[0] * (1 - a) + gl[k].r * a; c[1] = c[1] * (1 - a) + gl[k].g * a; c[2] = c[2] * (1 - a) + gl[k].b * a;
            }
            float d = B4[y & 3][x & 3] / 16.0f - 0.5f;   /* dither in units of one output LSB */
            int r = (int)lroundf(c[0] / 255.0f * 31 + d), g = (int)lroundf(c[1] / 255.0f * 63 + d), b = (int)lroundf(c[2] / 255.0f * 31 + d);
            r = r < 0 ? 0 : r > 31 ? 31 : r; g = g < 0 ? 0 : g > 63 ? 63 : g; b = b < 0 ? 0 : b > 31 ? 31 : b;
            px[y * UI_W + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
        if ((y & 31) == 31) vTaskDelay(1);
    }
    return px;
}

static lv_image_dsc_t s_wall_dsc2[2];                   /* 0 dark, 1 light */
static void wall_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    /* dark = row I: #0E0B1E to #050508, a violet glow top-left and a blue glow bottom-right */
    static const uint8_t dt[3] = {0x0E, 0x0B, 0x1E}, db[3] = {0x05, 0x05, 0x08}, lt[3] = {0xF3, 0xF1, 0xEC};
    static const glow_t dg[2] = {{48, 29, 120, 0.38f, 0x81, 0x5C, 0xFF}, {456, 470, 130, 0.34f, 0x0A, 0x84, 0xFF}};
    static const glow_t lg[4] = {{48, 19, 125, 0.85f, 0xFF, 0xC9, 0xA8}, {461, 48, 100, 0.80f, 0xD9, 0xCC, 0xFF},
                                 {442, 461, 140, 0.90f, 0xBF, 0xDB, 0xFF}, {19, 470, 110, 0.85f, 0xCD, 0xEF, 0xE0}};
    uint16_t *w[2] = {wall_make(dt, db, dg, 2), wall_make(lt, lt, lg, 4)};
    for (int k = 0; k < 2; k++)
        s_wall_dsc2[k] = (lv_image_dsc_t){ .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = UI_W, .h = UI_H,
                                                       .stride = UI_W * 2 }, .data_size = UI_W * UI_H * 2, .data = (const uint8_t *)w[k] };
    s_wall_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_wall_ready = w[0] && w[1];
    vTaskDelete(NULL);
}

/* ---------- launcher (H): one tileview. Tile 0 = Home (smart card, dark clock tile, SYSTEM card, All apps, dots, End
   drive pill). Tiles 1..4 = the apps, four a page in a 2 x 2 grid of lifted cards ---------- */
#define N_APP_PAGES 4
static lv_obj_t *s_tv, *s_tiles[1 + N_APP_PAGES];
static int s_ntiles, s_return_tile;
static lv_obj_t *h_ctl, *h_lab, *h_tag, *h_val, *h_unit, *h_dot, *h_line, *h_ind[3], *h_noclock, *w_hand_h, *w_hand_m;
static lv_obj_t *h_sub[4], *h_bar[3], *h_rv[4], *h_pw_note, *h_search, *h_pw_vi, *h_pw_chart, *h_pw_area, *h_pw_dot, *h_card;
static lv_chart_series_t *h_pw_ser, *h_pw_sera;
static lv_obj_t *h_sys;                                  /* the SYSTEM card (swipe step e4: redrawn after a slide) */
static int s_home_mode;
static void open_app(int idx);
static lv_obj_t *card_label(lv_obj_t *p, const char *t, uint32_t c);

/* a 60-point watt trace: an amber line over a translucent area (a bar series under it) */
static lv_obj_t *pw_chart(lv_obj_t *p, lv_chart_series_t **ser, lv_obj_t **area, lv_chart_series_t **sera, int h)
{
    lv_obj_t *ch[2];
    for (int j = 0; j < 2; j++) {
        ch[j] = lv_chart_create(p);
        lv_obj_set_height(ch[j], h);
        lv_chart_set_type(ch[j], j ? LV_CHART_TYPE_LINE : LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(ch[j], 60);
        lv_chart_set_axis_range(ch[j], LV_CHART_AXIS_PRIMARY_Y, 0, 160);
        lv_chart_set_div_line_count(ch[j], 0, 0);
        lv_obj_set_style_bg_opa(ch[j], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ch[j], 0, 0);
        lv_obj_set_style_pad_all(ch[j], 0, 0);
        lv_obj_set_style_size(ch[j], 0, 0, LV_PART_INDICATOR);
        lv_obj_set_style_line_width(ch[j], 2, LV_PART_ITEMS);
        lv_obj_set_style_pad_column(ch[j], 0, 0);
        lv_obj_set_style_pad_column(ch[j], 0, LV_PART_ITEMS);
        lv_obj_set_style_radius(ch[j], 0, LV_PART_ITEMS);
        if (!j) lv_obj_set_style_bg_opa(ch[j], LV_OPA_20, LV_PART_ITEMS);
        lv_obj_remove_flag(ch[j], LV_OBJ_FLAG_CLICKABLE);
        lv_chart_series_t *se = lv_chart_add_series(ch[j], lv_color_hex(HC_PW), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_values(ch[j], se, LV_CHART_POINT_NONE);
        if (j) *ser = se; else *sera = se;
    }
    *area = ch[0];
    return ch[1];
}

static void pw_fill(lv_obj_t *line, lv_chart_series_t *ser, lv_obj_t *area, lv_chart_series_t *sera, float *mx_out)
{
    float mx = 1.6f;
    for (int i = 0; i < s_pw_n; i++) if (s_pw_hist[i] > mx) mx = s_pw_hist[i];
    int32_t top = (int32_t)lroundf(mx * 100);
    lv_chart_set_axis_range(line, LV_CHART_AXIS_PRIMARY_Y, 0, top);
    lv_chart_set_axis_range(area, LV_CHART_AXIS_PRIMARY_Y, 0, top);
    for (int i = 0; i < 60; i++) {
        int j = i - (60 - s_pw_n);
        int32_t v = j >= 0 ? (int32_t)lroundf(s_pw_hist[j] * 100) : LV_CHART_POINT_NONE;
        lv_chart_set_value_by_id(line, ser, (uint32_t)i, v);
        lv_chart_set_value_by_id(area, sera, (uint32_t)i, v);
    }
    lv_chart_refresh(line);
    lv_chart_refresh(area);
    if (mx_out) *mx_out = mx;
}

/* the Power detail sheet: V, mA, W, the 60 s trace, min / avg / max, the gauge's time to empty; tap or swipe down closes */
static lv_obj_t *s_psheet;
static void psheet_close(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_BOTTOM) return;
    if (s_psheet) { lv_obj_delete_async(s_psheet); s_psheet = NULL; }
}
static void power_sheet_cb(lv_event_t *e)
{
    (void)e;
    if (s_psheet) return;
    const mosaico_gauge_t *g = &s_snap.gauge;
    s_psheet = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_psheet);
    lv_obj_set_size(s_psheet, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_psheet, lv_color_hex(UI_THEME.scrim), 0);
    lv_obj_set_style_bg_opa(s_psheet, LV_OPA_60, 0);
    lv_obj_add_flag(s_psheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_psheet, psheet_close, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_psheet, psheet_close, LV_EVENT_GESTURE, NULL);
    lv_obj_t *c = flex_box(s_psheet, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_pos(c, 22, 150);
    lv_obj_set_size(c, UI_W - 44, 300);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_THEME.sheet), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 24, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *r = flex_box(c, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 8);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    card_label(r, "POWER", UI_THEME.section[SEC_AMBER]);
    ui_label(r, UI_THEME.f_small, UI_MUTED, UI_SIM_BATT ? "SIMULATED  tap to close" : "tap to close");
    bool meas = g->ok && !(g->ma > -5 && g->ma < 5);
    char b[64];
    if (!g->ok) snprintf(b, sizeof b, "gauge not read");
    else if (!meas) snprintf(b, sizeof b, "%.2f V   on USB (current not measured)", g->mv / 1000.0);
    else snprintf(b, sizeof b, "%.2f V   %d mA   %.2f W", g->mv / 1000.0, abs(g->ma), g->mv * fabs((double)g->ma) / 1e6);
    ui_label(c, UI_THEME.f_value, UI_TEXT, b);
    lv_obj_t *area = NULL, *line;
    lv_chart_series_t *ser = NULL, *sera = NULL;
    line = pw_chart(c, &ser, &area, &sera, 130);
    lv_obj_set_width(line, LV_PCT(100));
    lv_obj_set_width(area, LV_PCT(100));
    lv_obj_add_flag(area, LV_OBJ_FLAG_IGNORE_LAYOUT);       /* the area sits under the line */
    float mx = 0;
    pw_fill(line, ser, area, sera, &mx);
    lv_obj_update_layout(line);
    lv_obj_set_pos(area, lv_obj_get_x(line), lv_obj_get_y(line));
    lv_obj_set_size(area, lv_obj_get_width(line), 130);
    float mn = 1e9f, sum = 0, mxx = 0;
    for (int i = 0; i < s_pw_n; i++) { mn = fminf(mn, s_pw_hist[i]); mxx = fmaxf(mxx, s_pw_hist[i]); sum += s_pw_hist[i]; }
    if (s_pw_n) snprintf(b, sizeof b, "last %d s: min %.2f  avg %.2f  max %.2f W", s_pw_n, (double)mn, (double)(sum / s_pw_n), (double)mxx);
    else snprintf(b, sizeof b, "no discharge measured in the last 60 s");
    ui_label(c, UI_THEME.f_label, UI_MUTED, b);
    if (g->ok && g->tte_min > 0 && g->tte_min < 65535)
        snprintf(b, sizeof b, "gauge time to empty %d min%s", g->tte_min, mosaico_gauge_uncalibrated(g) ? " (gauge not configured for the cell)" : "");
    else snprintf(b, sizeof b, "gauge time to empty: not reported");
    ui_label(c, UI_THEME.f_small, UI_MUTED, b);
}

static void card_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_home_mode != 4) return;                          /* the faults state opens Faults */
    for (int i = 0; i < ui_app_count(); i++) if (!strcmp(ui_app(i)->id, "faults")) { s_return_tile = 0; open_app(i); return; }
}
static lv_point_precise_t w_pts_h[2], w_pts_m[2];

static void open_app(int idx);
static void icon_cb(lv_event_t *e)
{
    s_return_tile = s_tv ? (int)lv_obj_get_index(lv_tileview_get_tile_active(s_tv)) : 0;
#if CONFIG_OCM_UI_PREFETCH
    if (pf_open_app((int)(intptr_t)lv_event_get_user_data(e))) return;   /* the app was built at the press */
#endif
    open_app((int)(intptr_t)lv_event_get_user_data(e));
}
static void all_apps_cb(lv_event_t *e) { (void)e; if (s_ntiles > 1) lv_tileview_set_tile_by_index(s_tv, 1, 0, LV_ANIM_ON); }

static int home_order(int *out)
{
    ui_settings_t set;
    ui_settings_get(&set);
    int n = 0;
    for (int i = 0; i < set.n; i++) out[n++] = set.order[i];
    return n;
}

static void page_dots(lv_obj_t *parent, int active, int n)
{
    lv_obj_t *row = flex_box(parent, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 6);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 10);
    for (int d = 0; d < n; d++) {
        lv_obj_t *dot = ui_rect(row, 0, 0, d == active ? 18 : 6, 6, d == active ? HT : HD, LV_RADIUS_CIRCLE);
        (void)dot;
    }
}

/* a page body below the status bar: a flex column from y top to 464 */
static lv_obj_t *page_col(lv_obj_t *tile, int top, int gap)
{
    lv_obj_t *c = flex_box(tile, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, gap);
    lv_obj_set_pos(c, 22, top);
    lv_obj_set_size(c, UI_W - 44, UI_H - 16 - top);
    return c;
}

static lv_obj_t *card_label(lv_obj_t *p, const char *t, uint32_t c)
{
    lv_obj_t *l = ui_label(p, UI_THEME.f_small, c, t);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

static void search_cb(lv_event_t *e) { (void)e; mosaico_board_request(MB_BLE_SEARCH, 0); }

static void build_home_tile(lv_obj_t *t)
{
    lv_obj_t *col = page_col(t, 56, 10);
    /* top row: smart card + clock tile */
    lv_obj_t *top = flex_box(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);
    lv_obj_set_size(top, LV_PCT(100), 160);
    lv_obj_t *sc = flex_box(top, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, 0);
    lv_obj_set_height(sc, 160);
    lv_obj_set_flex_grow(sc, 1);
    h_glass(sc, 22);
    lv_obj_set_style_pad_hor(sc, 16, 0);
    lv_obj_set_style_pad_ver(sc, 12, 0);
    h_card = sc;
    lv_obj_add_flag(sc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sc, card_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *r = flex_box(sc, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 6);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    h_lab = card_label(r, "ECONOMY NOW", HM);
    h_tag = ui_pill(r, "WAITING", HD);
    r = flex_box(sc, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, 8);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    h_val = ui_label(r, UI_THEME.f_hero, HT, "--");
    h_unit = ui_label(r, UI_THEME.f_label, HM, "");
    lv_obj_set_style_pad_bottom(h_unit, 8, 0);
    r = flex_box(sc, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 8);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    h_dot = ui_rect(r, 0, 0, 9, 9, HD, LV_RADIUS_CIRCLE);
    h_line = ui_label(r, UI_THEME.f_label, HM, "AI waits for a drive");
    lv_label_set_long_mode(h_line, LV_LABEL_LONG_DOT);
    lv_obj_set_height(h_line, lv_font_get_line_height(UI_THEME.f_label));   /* one line, cut with dots */
    lv_obj_set_flex_grow(h_line, 1);
    h_search = lv_obj_create(r);                          /* "Search": what the BOOT double-click does (elm_explore) */
    lv_obj_remove_style_all(h_search);
    lv_obj_set_size(h_search, 84, 44);
    h_glass(h_search, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(h_search, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h_search, search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(h_search, UI_THEME.f_label, HT, LV_SYMBOL_REFRESH " Search"));
    r = flex_box(sc, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 5);
    lv_obj_set_size(r, LV_SIZE_CONTENT, 4);
    for (int k = 0; k < 3; k++) h_ind[k] = ui_rect(r, 0, 0, k ? 4 : 16, 4, k ? HD : HT, LV_RADIUS_CIRCLE);
    /* dark clock tile: ring, amber bars, grey dots, light hands, amber centre */
    lv_obj_t *ct = ui_rect(top, 0, 0, 160, 160, 0, 22);
    h_glass(ct, 22);
    lv_obj_t *face = ui_rect(ct, 16, 16, 128, 128, 0, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face, 2, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(s_light ? 0xC7C7CC : 0x2A2A2E), 0);   /* Row I ring */
    for (int i = 0; i < 12; i++) {
        float a = (float)i * (float)M_PI / 6.0f;
        int cx = 64 + (int)lroundf(52 * sinf(a)), cy = 64 - (int)lroundf(52 * cosf(a));
        if (i % 3 == 0) ui_rect(face, cx - (i % 6 ? 6 : 2), cy - (i % 6 ? 2 : 6), i % 6 ? 12 : 4, i % 6 ? 4 : 12, 0xF59A2E, 2);
        else ui_rect(face, cx - 2, cy - 2, 4, 4, HD, LV_RADIUS_CIRCLE);
    }
    w_hand_m = lv_line_create(face);
    lv_obj_set_style_line_color(w_hand_m, lv_color_hex(HT), 0);
    lv_obj_set_style_line_width(w_hand_m, 3, 0);
    lv_obj_set_style_line_rounded(w_hand_m, true, 0);
    w_hand_h = lv_line_create(face);
    lv_obj_set_style_line_color(w_hand_h, lv_color_hex(HT), 0);
    lv_obj_set_style_line_width(w_hand_h, 5, 0);
    lv_obj_set_style_line_rounded(w_hand_h, true, 0);
    ui_rect(face, 59, 59, 10, 10, 0xF59A2E, LV_RADIUS_CIRCLE);
    h_noclock = ui_label(face, UI_THEME.f_small, HM, "no clock yet");
    lv_obj_align(h_noclock, LV_ALIGN_CENTER, 0, 28);
    /* SYSTEM card: a header row, then one lv grid: label 78 px | bar (fr) | value 84 px right-aligned, rows centred */
    lv_obj_t *sy = h_sys = flex_box(col, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, 0);
    lv_obj_set_width(sy, LV_PCT(100));
    lv_obj_set_flex_grow(sy, 1);
    h_glass(sy, 22);
    lv_obj_set_style_pad_hor(sy, 16, 0);
    lv_obj_set_style_pad_ver(sy, 8, 0);
    r = flex_box(sy, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 6);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    card_label(r, "SYSTEM", HC_CPU);
    ui_label(r, UI_THEME.f_small, HM, "ESP32-S31, 2 cores");
    /* one lv grid: label 106 px | bar (fr) | value 70 px right-aligned, column gap 16, rows at least 34 px, all centred */
    lv_obj_t *g = lv_obj_create(sy);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
    static const int32_t gc[] = {106, LV_GRID_FR(1), 70, LV_GRID_TEMPLATE_LAST};
    static const int32_t gr[] = {34, 34, 34, 34, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(g, gc, gr);
    lv_obj_set_style_pad_column(g, 16, 0);
    lv_obj_set_style_pad_row(g, 0, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    static const char *const RN[4] = {"CPU", "Core 0", "Core 1", "Power"};
    const uint32_t RC[4] = {HC_CPU, HC_C0, HC_C1, HC_PW};
    for (int k = 0; k < 4; k++) {
        lv_obj_t *lc = flex_box(g, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, 1);
        lv_obj_set_size(lc, 106, LV_SIZE_CONTENT);
        lv_obj_set_grid_cell(lc, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, k, 1);
        ui_label(lc, UI_THEME.f_label, HT, RN[k]);
        h_sub[k] = ui_label(lc, UI_THEME.f_small, k == 3 ? HC_PW : HM, k == 0 ? "BOTH CORES" : "");
        lv_label_set_long_mode(h_sub[k], LV_LABEL_LONG_CLIP);              /* nowrap: "SYS 5.4 MIPS" is the widest */
        lv_obj_set_style_text_letter_space(h_sub[k], 1, 0);
        if (k == 3) { h_pw_vi = ui_label(lc, UI_THEME.f_small, HM, ""); lv_label_set_long_mode(h_pw_vi, LV_LABEL_LONG_CLIP); }
        if (k < 3) {
            lv_obj_t *bb = lv_bar_create(g);
            lv_obj_set_height(bb, 6);
            lv_bar_set_range(bb, 0, 100);
            lv_obj_set_style_bg_color(bb, lv_color_hex(HTRACK), 0);
            lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(bb, lv_color_hex(RC[k]), LV_PART_INDICATOR);
            lv_obj_remove_flag(bb, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_grid_cell(bb, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, k, 1);
            h_bar[k] = bb;
        } else {
            h_pw_note = ui_label(g, UI_THEME.f_small, HM, "");
            lv_label_set_long_mode(h_pw_note, LV_LABEL_LONG_DOT);
            lv_obj_set_grid_cell(h_pw_note, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, k, 1);
            h_pw_chart = pw_chart(g, &h_pw_ser, &h_pw_area, &h_pw_sera, 30);
            lv_obj_set_grid_cell(h_pw_area, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, k, 1);
            lv_obj_set_grid_cell(h_pw_chart, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, k, 1);
            h_pw_dot = ui_rect(g, 0, 0, 6, 6, HC_PW, LV_RADIUS_CIRCLE);
            lv_obj_add_flag(h_pw_dot, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
        }
        h_rv[k] = ui_label(g, UI_THEME.f_value, RC[k], "--");
        lv_obj_set_style_text_align(h_rv[k], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_grid_cell(h_rv[k], LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_CENTER, k, 1);
    }
    lv_obj_t *tap = lv_obj_create(g);                     /* the whole Power row is the touch target for the detail sheet */
    lv_obj_remove_style_all(tap);
    lv_obj_set_grid_cell(tap, LV_GRID_ALIGN_STRETCH, 0, 3, LV_GRID_ALIGN_STRETCH, 3, 1);
    lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tap, power_sheet_cb, LV_EVENT_CLICKED, NULL);
    /* bottom row: All apps, dots, End drive; inset from the rounded corners */
    r = flex_box(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 8);
    lv_obj_set_size(r, LV_PCT(100), 44);
    lv_obj_set_style_pad_hor(r, 18, 0);
    lv_obj_t *aa = flex_box(r, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 6);
    lv_obj_set_size(aa, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(aa, 14, 0);
    h_glass(aa, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(aa, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(aa, all_apps_cb, LV_EVENT_CLICKED, NULL);
    ui_label(aa, UI_THEME.f_label, HT, LV_SYMBOL_LIST "  All apps");
    page_dots(r, 0, s_ntiles);
    h_ctl = ui_drive_pill(r, 0, 0, 168, 44);
}

static void refresh_clock(const ui_snap_t *s)
{
    bool have = mosaico_clock_known(s->st);
    show(w_hand_h, have); show(w_hand_m, have); show(h_noclock, !have);
    if (!have) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    float am = (float)tm.tm_min * (float)M_PI / 30.0f, ah = ((float)(tm.tm_hour % 12) + tm.tm_min / 60.0f) * (float)M_PI / 6.0f;
    w_pts_m[0] = (lv_point_precise_t){ 64, 64 };
    w_pts_m[1] = (lv_point_precise_t){ 64 + 46 * sinf(am), 64 - 46 * cosf(am) };
    w_pts_h[0] = (lv_point_precise_t){ 64, 64 };
    w_pts_h[1] = (lv_point_precise_t){ 64 + 28 * sinf(ah), 64 - 28 * cosf(ah) };
    lv_line_set_points(w_hand_m, w_pts_m, 2);
    lv_line_set_points(w_hand_h, w_pts_h, 2);
}

static void home_tag(const char *t, uint32_t c)
{
    ui_set_text(h_tag, t);
    ui_set_color(h_tag, c);
    lv_obj_set_style_border_color(h_tag, lv_color_hex(c), 0);
}

EXT_RAM_BSS_ATTR static md_summary_t s_home_sum;
static bool have_level(const ui_snap_t *s) { return s->obd_live && OBD_HAS(&s->st->obd, F_fuel_level); }

static void refresh_home(const ui_snap_t *s)
{
#if CONFIG_OCM_SWIPE_INTENT
    uint32_t mut0 = s_ui_mut;
#endif
    const obd_values_t *o = &s->st->obd;
    char v[24], line[64];
    int mode;                                             /* 0 economy, 1 last drive, 2 fuel low, 3 waiting */
    const rev_tank_out_t *tk = &s->rs.tank;
    float level = have_level(s) ? o->f[F_fuel_level] : -1;
    mosaico_drive_get_summary(&s_home_sum);
    float l100 = s_home_sum.ok && s_home_sum.l100 > 0 ? s_home_sum.l100 : o->econ_trip_kpl > 0.05f ? 100.0f / o->econ_trip_kpl : -1;
    float tank_l = tk->have ? tk->cap_raw_l * s->rs.cal : -1;
    const fault_report_t *fr = &s->st->faults;
    if (fr->n && (fr->max_severity >= SEV_HIGH || fr->mil_on || o->mil_on)) mode = 4;
    else if (level >= 0 && level < 15 && tank_l > 0 && l100 > 0) mode = 2;
    else if (s->obd_live && s->drive_active) mode = 0;
    else if (s_home_sum.ok) mode = 1;
    else mode = 3;
    s_home_mode = mode;
    if (mode == 4) {
        const fault_t *f = &fr->faults[0];
        ui_set_fmt(h_lab, "%d FAULT%s", fr->n, fr->n == 1 ? "" : "S");
        home_tag(fault_severity_name(fr->max_severity), HC_RED);
        ui_set_text(h_val, f->code); ui_set_color(h_val, HC_RED); ui_set_text(h_unit, "");
        ui_set_bg(h_dot, HC_RED);
        ui_set_text(h_line, f->desc); ui_set_color(h_line, HT);
    } else if (mode == 0) {
        ui_set_text(h_lab, "ECONOMY NOW");
        const char *u = ui_econ(o->econ_inst_kpl > 0 ? o->econ_inst_kpl : -1, v, sizeof v);
        ui_set_text(h_val, v); ui_set_color(h_val, HT); ui_set_text(h_unit, u);
        if (o->fuel_lph_calc >= 0) {
            char up[12]; const char *src = fuel_src_name((fuel_src_t)o->fuel_src); int i = 0;
            for (; src[i] && i < 11; i++) up[i] = (char)toupper((unsigned char)src[i]);
            up[i] = 0;
            home_tag(up, HC_PW);
        } else home_tag("NO SOURCE", HD);
        int k = s->ml.last_class;
        ui_set_bg(h_dot, k >= 0 && k < 4 ? UI_THEME.cls[k] : HD);
        if (k >= 0) snprintf(line, sizeof line, "%s, %d %% sure", ui_class_name(k), (int)lroundf(s->ml.last_prob * 100));
        else snprintf(line, sizeof line, "AI waits for the first window");
        ui_set_text(h_line, line); ui_set_color(h_line, HT);
    } else if (mode == 1) {
        const md_summary_t *m = &s_home_sum;
        ui_set_text(h_lab, "LAST DRIVE");
        if (m->start_epoch_ms > 1600000000000LL) {
            time_t t0 = (time_t)(m->start_epoch_ms / 1000); struct tm a; localtime_r(&t0, &a);
            snprintf(v, sizeof v, "%02d:%02d", a.tm_hour, a.tm_min); home_tag(v, HC_SKY);
        } else { snprintf(v, sizeof v, "s%06lu", (unsigned long)m->session); home_tag(v, HC_SKY); }
        snprintf(v, sizeof v, "%.1f", (double)m->km);
        ui_set_text(h_val, v); ui_set_color(h_val, HC_SKY);
        ui_set_fmt(h_unit, "km, %ld min", (long)(m->dur_s / 60));
        ui_set_bg(h_dot, HC_SKY);
        char e[16];
        const char *u = ui_econ(m->l100 > 0 ? 100.0f / m->l100 : -1, e, sizeof e);
        snprintf(line, sizeof line, "%s %s, %d hard brake%s", e, u, m->hard_brakes, m->hard_brakes == 1 ? "" : "s");
        ui_set_text(h_line, line); ui_set_color(h_line, HT);
    } else if (mode == 2) {
        ui_set_text(h_lab, "FUEL LOW");
        snprintf(v, sizeof v, "%.0f %%", (double)level); home_tag(v, HC_RED);
        float km = level / 100.0f * tank_l / (l100 / 100.0f);
        snprintf(v, sizeof v, "%.0f", (double)km);
        ui_set_text(h_val, v); ui_set_color(h_val, HC_WARN); ui_set_text(h_unit, "km left");
        ui_set_bg(h_dot, HC_RED);
        snprintf(line, sizeof line, "At your %.1f L/100 km average", (double)l100);
        ui_set_text(h_line, line); ui_set_color(h_line, HT);
    } else {
        ui_set_text(h_lab, "ECONOMY NOW");
        home_tag("WAITING", HD);
        ui_set_text(h_val, "--"); ui_set_color(h_val, HD);
        char e[4]; ui_set_text(h_unit, ui_econ(-1, e, sizeof e));
        ui_set_bg(h_dot, HD);
        const app_status_t *st0 = s->st;
        bool prompt = st0->prompt_text[0] && s->now_ms < st0->prompt_until_ms;
        bool linked = st0->ble == BLE_ST_LOGGING;
        ui_set_text(h_line, prompt || linked ? "No engine data" : "No car yet");
        ui_set_color(h_line, prompt ? UI_WARN : HM);
    }
    int ind = mode == 3 || mode == 4 ? 0 : mode;
    for (int k = 0; k < 3; k++) { lv_obj_set_width(h_ind[k], k == ind ? 16 : 4); ui_set_bg(h_ind[k], k == ind ? HT : HD); }
    refresh_clock(s);
    /* SYSTEM card */
    const app_status_t *st = s->st;
    int pc[3] = {(st->cpu_pct[0] + st->cpu_pct[1] + 1) / 2, st->cpu_pct[0], st->cpu_pct[1]};
    for (int k = 0; k < 3; k++) {
        if (lv_bar_get_value(h_bar[k]) != pc[k]) lv_bar_set_value(h_bar[k], pc[k], LV_ANIM_OFF);
        ui_set_fmt(h_rv[k], "%d %%", pc[k]);
    }
    if (st->mips_ok) { ui_set_fmt(h_sub[1], "SYS  %.1f MIPS", (double)st->pmu_mips[0]); ui_set_fmt(h_sub[2], "AI  %.1f MIPS", (double)st->pmu_mips[1]); }
    else { ui_set_text(h_sub[1], "SYS"); ui_set_text(h_sub[2], "AI"); }
    bool meas;
    char pw[16];
    power_text(s, pw, sizeof pw, &meas);
    ui_set_text(h_rv[3], pw);
    show(h_pw_note, !meas);
    show(h_pw_chart, meas); show(h_pw_area, meas); show(h_pw_vi, meas); show(h_pw_dot, meas && s_pw_n > 0);
    if (!s->gauge.ok) { ui_set_text(h_sub[3], ""); ui_set_text(h_pw_note, "gauge not read"); }
    else if (!meas) { ui_set_text(h_sub[3], "ON USB"); ui_set_text(h_pw_note, "USB current is not measured"); }
    else {
        ui_set_text(h_sub[3], UI_SIM_BATT ? "BATTERY (SIM)" : "BATTERY");
        ui_set_fmt(h_pw_vi, "%.2f V x %d mA", s->gauge.mv / 1000.0, abs(s->gauge.ma));
        pw_fill(h_pw_chart, h_pw_ser, h_pw_area, h_pw_sera, NULL);
        SI_MUT();
        if (s_pw_n) {                                      /* the dot on the newest point */
            lv_point_t pt;
            lv_obj_update_layout(h_pw_chart);
            lv_chart_get_point_pos_by_id(h_pw_chart, h_pw_ser, 59, &pt);
            lv_obj_set_pos(h_pw_dot, lv_obj_get_x(h_pw_chart) + pt.x - 3, lv_obj_get_y(h_pw_chart) + pt.y - 3);
        }
    }
    show(h_search, mode == 3);
    ui_drive_ctl_refresh(h_ctl, s);
#if CONFIG_OCM_SWIPE_INTENT
    if (s_ui_mut != mut0) s_home_gen++;                   /* Home changed: its slide picture is no longer current */
#endif
}

/* one apps page: title row, 2 x 2 lifted cards with a 100 px flat icon (radius 26, dark glyph) and an 18 px label */
static void build_apps_page(lv_obj_t *t, int page, const int *apps, int n)
{
    static const char *const TITLE[N_APP_PAGES] = {"DRIVE AND AI", "CAR DATA", "TRIP AND SYSTEM", "FILES, WI-FI, SETTINGS"};
    lv_obj_t *col = page_col(t, 56, 10);
    lv_obj_t *r = flex_box(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(r, 8, 0);
    card_label(r, TITLE[page < N_APP_PAGES ? page : N_APP_PAGES - 1], HM);
    lv_obj_t *pl = ui_label(r, UI_THEME.f_small, HM, "");
    ui_set_fmt(pl, "%d of %d", page + 1, N_APP_PAGES);
    lv_obj_t *grid = lv_obj_create(col);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    static const int32_t cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, cols, rows);
    lv_obj_set_style_pad_gap(grid, 12, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (int k = 0; k < n && k < 4; k++) {
        int app = apps[k];
        lv_obj_t *cell = flex_box(grid, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 10);
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, k % 2, 1, LV_GRID_ALIGN_STRETCH, k / 2, 1);
        h_glass(cell, 28);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, icon_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app);
#if CONFIG_OCM_UI_METRICS
        if (s_nicons < UM_MAX_ICONS) { s_icon_cell[s_nicons] = cell; s_icon_app[s_nicons++] = (uint8_t)app; }
#endif
#if CONFIG_OCM_UI_PREFETCH
        lv_obj_add_event_cb(cell, pf_cell_pressing_cb, LV_EVENT_PRESSING, NULL);
#endif
        /* Tabler icon (A8, recoloured): dark = bright tile with a dark glyph, light = deeper tile with a white glyph */
        if (ui_app(app)->tile) {                          /* the app's own mark is the tile (98 px square, centred in the 100 px slot) */
            lv_obj_t *box = flex_box(cell, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 0);
            lv_obj_set_size(box, 100, 100);
            lv_obj_t *mk = lv_image_create(box);
            lv_image_set_src(mk, ui_app(app)->tile);
        } else {
            lv_obj_t *ic = ui_rect(cell, 0, 0, 100, 100, s_light ? ui_app(app)->icon_g1 : ui_app(app)->icon_bg, 26);
            lv_obj_t *gl = lv_image_create(ic);
            lv_image_set_src(gl, ui_app(app)->icon52);
            lv_obj_set_style_image_recolor(gl, lv_color_hex(s_light ? 0xFFFFFF : 0x0B0B0D), 0);
            lv_obj_set_style_image_recolor_opa(gl, LV_OPA_COVER, 0);
            lv_obj_center(gl);
        }
        ui_label(cell, UI_THEME.f_status, HT, ui_app(app)->name);
    }
    lv_obj_t *dr = flex_box(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_size(dr, LV_PCT(100), 10);
    page_dots(dr, 1 + page, 1 + N_APP_PAGES);
}

static void slide_attach(void);
static void build_launcher(void)
{
    lv_obj_clean(s_home);
    lv_obj_set_style_bg_color(s_home, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(s_home, LV_GRAD_DIR_NONE, 0);
    s_wall_img = lv_image_create(s_home);              /* behind everything; the source is set when the wallpaper is ready */
    lv_obj_set_pos(s_wall_img, 0, 0);
    lv_obj_set_size(s_wall_img, UI_W, UI_H);
    if (s_wall_ready) lv_image_set_src(s_wall_img, &s_wall_dsc2[s_light]);
    lv_obj_set_style_bg_color(s_home, lv_color_hex(s_light ? 0xF3F1EC : 0x000000), 0);
    int shown[UI_MAX_APPS], n = home_order(shown);
    s_ntiles = 1 + N_APP_PAGES;
#if CONFIG_OCM_UI_METRICS
    s_nicons = 0;
#endif
    s_tv = lv_tileview_create(s_home);
    lv_obj_set_size(s_tv, UI_W, UI_H);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    for (int t = 0; t < s_ntiles; t++) {
        s_tiles[t] = lv_tileview_add_tile(s_tv, (uint8_t)t, 0, (t > 0 ? LV_DIR_LEFT : 0) | (t < s_ntiles - 1 ? LV_DIR_RIGHT : 0));
        lv_obj_remove_flag(s_tiles[t], LV_OBJ_FLAG_SCROLLABLE);
    }
    build_home_tile(s_tiles[0]);
    for (int p = 0; p < N_APP_PAGES; p++) build_apps_page(s_tiles[1 + p], p, shown + p * 4, n - p * 4 > 0 ? n - p * 4 : 0);
    if (s_return_tile >= s_ntiles) s_return_tile = 0;
    lv_tileview_set_tile_by_index(s_tv, (uint32_t)s_return_tile, 0, LV_ANIM_OFF);
    slide_attach();
}

/* ---------- snapshot sliding (swipe step a): while the tileview scrolls, each page is one cached RGB565 picture of the
   page over the wallpaper, and the live widgets and the screen wallpaper are hidden, so a frame is two image copies
   instead of the wallpaper plus every glass card. Apps pages are static: their pictures are taken once after a build,
   one per UI tick. Home changes every second, so its picture is retaken when a slide starts. ---------- */
#define UI_SNAP_SLIDE 1
#if UI_SNAP_SLIDE
static lv_draw_buf_t s_tsnap[1 + N_APP_PAGES];
static uint8_t *s_tsnap_mem[1 + N_APP_PAGES];
static bool s_tsnap_ok[1 + N_APP_PAGES], s_sliding;
static lv_obj_t *s_tsnap_img[1 + N_APP_PAGES], *s_tile_wall[1 + N_APP_PAGES];

/* ---------- swipe step e: direct panel blit. While the tileview slides LVGL draws nothing (invalidation off); each
   scroll position is composed from two byte-swapped page pictures into one of two PSRAM frames (row memcpy), written
   back from the cache and sent in one window by PSRAM DMA (raw link test: 23.6 ms a full frame). Rows above BLIT_Y0
   keep the status bar LVGL drew last. At rest LVGL redraws the screen once. ---------- */
#define UI_BLIT_SLIDE 1
#define BLIT_Y0 48
#define BLIT_ROWS (UI_H - BLIT_Y0)
#if UI_BLIT_SLIDE
static uint16_t *s_tblit[1 + N_APP_PAGES], *s_bfb[2];
static int s_bfi;
static uint32_t s_bl_n, s_bl_comp_us, s_bl_send_us, s_bl_comp_max, s_bl_send_max;   /* blit cost split, for the swipe line */
static bool s_blit_on;

static void blit_prepare(int i)
{
    size_t n = (size_t)UI_W * BLIT_ROWS;
    if (!s_tblit[i]) s_tblit[i] = heap_caps_aligned_alloc(64, n * 2, MALLOC_CAP_SPIRAM);
    if (!s_tblit[i]) return;
    uint32_t st = s_tsnap[i].header.stride / 2;
    for (int r = 0; r < BLIT_ROWS; r++) {
        const uint16_t *src = (const uint16_t *)s_tsnap[i].data + (size_t)(r + BLIT_Y0) * st;
        uint16_t *dst = s_tblit[i] + (size_t)r * UI_W;
        for (int x = 0; x < UI_W; x++) { uint16_t v = src[x]; dst[x] = (uint16_t)((v >> 8) | (v << 8)); }  /* panel byte order */
    }
}

static bool blit_ready(void)
{
    for (int k = 0; k < 2; k++)
        if (!s_bfb[k]) s_bfb[k] = heap_caps_aligned_alloc(64, (size_t)UI_W * BLIT_ROWS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_bfb[0] || !s_bfb[1]) return false;
    for (int i = 0; i < s_ntiles; i++) if (!s_tblit[i]) return false;
    return true;
}

static void blit_frame(int32_t x)
{
    int64_t t0 = esp_timer_get_time();
    int32_t maxx = (s_ntiles - 1) * UI_W;
    if (x < 0) x = 0;
    if (x > maxx) x = maxx;
    int a = (int)(x / UI_W), off = (int)(x % UI_W);
    uint16_t *fb = s_bfb[s_bfi];
    const uint16_t *pa = s_tblit[a], *pb = off ? s_tblit[a + 1] : NULL;
    for (int r = 0; r < BLIT_ROWS; r++) {
        memcpy(fb + (size_t)r * UI_W, pa + (size_t)r * UI_W + off, (size_t)(UI_W - off) * 2);
        if (off) memcpy(fb + (size_t)r * UI_W + (UI_W - off), pb + (size_t)r * UI_W, (size_t)off * 2);
    }
    esp_cache_msync(fb, (size_t)UI_W * BLIT_ROWS * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    int64_t t1 = esp_timer_get_time();
    esp_lcd_panel_draw_bitmap(s_panel, 0, BLIT_Y0, UI_W, UI_H, fb);   /* its window commands wait for the previous frame */
    s_bfi ^= 1;
    int64_t now = esp_timer_get_time();
    uint32_t c = (uint32_t)(t1 - t0), sd = (uint32_t)(now - t1);
    s_bl_n++; s_bl_comp_us += c; s_bl_send_us += sd;
    if (c > s_bl_comp_max) s_bl_comp_max = c;
    if (sd > s_bl_send_max) s_bl_send_max = sd;
    if ((uint32_t)(now - t0) > s_render_max_us) s_render_max_us = (uint32_t)(now - t0);
    UM_FRAME();
}
#endif

static int64_t s_home_snap_us;                           /* when the Home picture was taken */
static bool tile_snap(int i)
{
    if (i == 0) s_home_snap_us = esp_timer_get_time();
    if (!s_wall_ready || !s_tiles[i]) return false;
    uint32_t stride = lv_draw_buf_width_to_stride(UI_W, LV_COLOR_FORMAT_RGB565);
    if (!s_tsnap_mem[i]) {
        s_tsnap_mem[i] = heap_caps_aligned_alloc(64, stride * UI_H, MALLOC_CAP_SPIRAM);
        if (!s_tsnap_mem[i]) return false;
    }
    lv_draw_buf_init(&s_tsnap[i], UI_W, UI_H, LV_COLOR_FORMAT_RGB565, stride, s_tsnap_mem[i], stride * UI_H);
    int64_t t0 = esp_timer_get_time();
    lv_image_set_src(s_tile_wall[i], &s_wall_dsc2[s_light]);
    lv_obj_remove_flag(s_tile_wall[i], LV_OBJ_FLAG_HIDDEN);   /* the page's own copy of the wallpaper is the picture's background */
    lv_obj_add_flag(s_tsnap_img[i], LV_OBJ_FLAG_HIDDEN);
    lv_result_t r = lv_snapshot_take_to_draw_buf(s_tiles[i], LV_COLOR_FORMAT_RGB565, &s_tsnap[i]);
    lv_obj_add_flag(s_tile_wall[i], LV_OBJ_FLAG_HIDDEN);
    s_tsnap_us = (uint32_t)(esp_timer_get_time() - t0);
    s_tsnap_ok[i] = r == LV_RESULT_OK;
#if CONFIG_OCM_SWIPE_INTENT
    if (i == 0 && s_tsnap_ok[0]) s_home_pic_gen = s_home_gen;
#endif
    if (s_tsnap_ok[i]) lv_image_set_src(s_tsnap_img[i], &s_tsnap[i]);
#if UI_BLIT_SLIDE
    if (s_tsnap_ok[i]) blit_prepare(i);
#endif
    return s_tsnap_ok[i];
}

static void slide_set(bool on)
{
    for (int i = 0; i < s_ntiles; i++) {
        uint32_t n = lv_obj_get_child_count(s_tiles[i]);
        for (uint32_t k = 0; k < n; k++) {
            lv_obj_t *c = lv_obj_get_child(s_tiles[i], (int32_t)k);
            if (c == s_tsnap_img[i] || c == s_tile_wall[i]) continue;
            if (on && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_USER_1);
            else if (!on && lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_USER_1);
        }
        if (on) lv_obj_remove_flag(s_tsnap_img[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_tsnap_img[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (on) lv_obj_add_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(s_wall_img, LV_OBJ_FLAG_HIDDEN);
    s_sliding = on;
}

static void tv_scroll_cb(lv_event_t *e)
{
#if UI_BLIT_SLIDE
    if (lv_event_get_code(e) == LV_EVENT_SCROLL) {
        if (s_blit_on) blit_frame(lv_obj_get_scroll_x(s_tv));
        return;
    }
#endif
#if CONFIG_OCM_UI_PREFETCH
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) pf_act(upf_scroll_begin(&s_pf));   /* the press became a swipe */
#endif
    if (lv_event_get_code(e) != LV_EVENT_SCROLL_BEGIN || s_sliding) return;
    if (!s_tsnap_ok[0] || esp_timer_get_time() - s_home_snap_us > 10000000) s_tsnap_ok[0] = false;   /* e4: a picture up to 5 s old is fine */
    for (int i = 0; i < s_ntiles; i++) if (!s_tsnap_ok[i] && !tile_snap(i)) return;   /* no picture: slide live */
#if UI_BLIT_SLIDE
    if (blit_ready()) {
        lv_display_enable_invalidation(s_disp, false);    /* LVGL draws nothing until the slide ends */
        s_blit_on = s_sliding = true;
        blit_frame(lv_obj_get_scroll_x(s_tv));
        return;
    }
#endif
    slide_set(true);
}

/* from the UI tick: end the picture mode once the tileview rests on a page and no finger drags it; fill missing pictures */
static void slide_tick(void)
{
    if (!s_tv) return;
    lv_indev_t *id = lv_indev_get_next(NULL);
    bool dragging = id && lv_indev_get_scroll_obj(id) == s_tv;
    bool moving = lv_anim_get(s_tv, NULL) != NULL || dragging || lv_obj_get_scroll_x(s_tv) % UI_W != 0;
#if UI_BLIT_SLIDE
    if (s_blit_on && !moving) {
        s_blit_on = s_sliding = false;
        lv_display_enable_invalidation(s_disp, true);
        /* e4: the panel already shows the page's picture. Redraw only the status bar and, on Home, the two cards whose
           values may have changed while invalidation was off. */
        lv_obj_invalidate(s_sb);
        if (lv_obj_get_scroll_x(s_tv) / UI_W == 0 && !SI_HOME_PIC_CURRENT()) {   /* smart swipe: a current picture needs no redraw */
            if (h_card) lv_obj_invalidate(h_card);
            if (h_sys) lv_obj_invalidate(h_sys);
        }
    }
#endif
    if (s_sliding && !moving) slide_set(false);
    /* missing pictures (after a rebuild) are taken only while the launcher is on screen: a Settings change made in an
       app costs no picture work there (measured before S1: 4-5 pictures of 40-195 ms each right after a tap) */
    if (!s_sliding && !moving && s_cur < 0 && lv_screen_active() == s_home) {
        /* fill missing pictures one per tick; Home once too, so the first slide after boot does not pay for the buffers */
        static bool home_warm;
        int todo = -1;
        for (int i = 1; i < s_ntiles && todo < 0; i++) if (!s_tsnap_ok[i]) todo = i;
        if (todo < 0 && !home_warm && s_wall_ready) { todo = 0; home_warm = true; }
        if (todo >= 0) tile_snap(todo);
#if UI_BLIT_SLIDE
        else { static bool frames_warm; if (!frames_warm) frames_warm = blit_ready(); }
#endif
    }
}

/* e4: keep the Home picture at most 5 s old while Home rests, on its own LVGL timer (not the "ui" sched job), so a slide
   from Home starts at once instead of after a 120 ms picture */
static void home_pic_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_tv || s_sliding || !s_wall_ready || lv_screen_active() != s_home) return;
    lv_indev_t *id = lv_indev_get_next(NULL);
    if (lv_anim_get(s_tv, NULL) || (id && lv_indev_get_scroll_obj(id) == s_tv) || lv_obj_get_scroll_x(s_tv) != 0) return;
    if (esp_timer_get_time() - s_home_snap_us >= 5000000) tile_snap(0);
}

static void slide_attach(void)
{
    static lv_timer_t *home_pic;
    if (!home_pic) home_pic = lv_timer_create(home_pic_cb, 1000, NULL);
    s_sliding = false;
    for (int i = 0; i < s_ntiles; i++) {
        s_tsnap_ok[i] = false;
        s_tile_wall[i] = lv_image_create(s_tiles[i]);
        lv_obj_add_flag(s_tile_wall[i], LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_tile_wall[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_tile_wall[i], 0, 0);
        lv_obj_move_to_index(s_tile_wall[i], 0);
        s_tsnap_img[i] = lv_image_create(s_tiles[i]);
        lv_obj_add_flag(s_tsnap_img[i], LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_tsnap_img[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_tsnap_img[i], 0, 0);
    }
    lv_obj_add_event_cb(s_tv, tv_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
#if UI_BLIT_SLIDE
    lv_obj_add_event_cb(s_tv, tv_scroll_cb, LV_EVENT_SCROLL, NULL);
#if CONFIG_OCM_SWIPE_INTENT
    si_attach(s_tv);
#endif
    s_blit_on = false;
#endif
}
#else
static void slide_tick(void) {}
static void slide_attach(void) {}
#endif

#if CONFIG_OCM_SWIPE_INTENT && UI_SNAP_SLIDE && UI_BLIT_SLIDE
/* ---------- smart swipe: the swipe_intent learner on the launcher's tileview ----------
   Touch samples reach the learner from touch_read_cb (LVGL task, before LVGL processes them); it predicts the outcome
   (next / previous / stay) 40 ms after touch-down. Nothing here triggers an action: the tileview follows the finger 1:1
   as before and LVGL alone picks the page. On arm-A gestures (si_acts) the firmware may only
     (a) keep the Home picture current while the user rests on a page from which the page prior says Home comes next,
         and skip the Home card redraw after a slide when the picture is already current;
     (b) at the lift (LV_EVENT_SCROLL_THROW_BEGIN), run LVGL's own snap decision at once instead of on the next
         animation tick, when the learner's page agrees with where LVGL's throw lands.
   Arm B predicts and measures only. Settled = the snap's SCROLL_END if the destination's picture was current, else the
   end of the corrective redraw (the next RENDER_READY after slide_tick re-enables invalidation). */
EXT_RAM_BSS_ATTR static uint8_t s_si_blob[SI_BLOB_BYTES];
static si_t s_si;                                      /* 1296 B, internal RAM: the RLS loop stays out of PSRAM */
static bool s_si_on, s_si_gest, s_si_lifted;
static int s_si_wait = -1;                             /* page whose corrective redraw ends the settle, -1 none */
static uint8_t s_si_page0, s_si_state_saved = 0xFF;
static si_pred_t s_si_pred;
static uint32_t s_si_tag, s_si_n, s_si_early, s_si_warm, s_si_cyc_move, s_si_cyc_settle;
static char s_si_brief[40];
static portMUX_TYPE s_si_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t si_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* page i's slide picture shows what the live page would (apps pages are static; Home changes with its content) */
static bool si_fresh(int i) { return i >= 0 && i < s_ntiles && s_tsnap_ok[i] && (i != 0 || s_home_pic_gen == s_home_gen); }

static uint8_t si_fresh_mask(void)
{
    uint8_t m = 0;
    for (int i = 0; i < s_ntiles && i < SI_MAX_PAGES; i++) if (si_fresh(i)) m |= (uint8_t)(1u << i);
    return m;
}

static void si_log(const char *why)
{
    char l[96];
    si_metrics_t m;
    si_metrics_line(&s_si, l, sizeof l);
    si_metrics_get(&s_si, &m);
    ESP_LOGW(TAG, "%s: %s | z %.2f, acc %.1f %%, updates %lu, resets %u, early snaps %lu, warm pictures %lu, worst cycles "
             "predict %lu settle %lu (%.1f / %.1f us)", why, l, m.z100 / 100.0, m.acc_pm / 10.0, (unsigned long)m.updates,
             m.resets, (unsigned long)s_si_early, (unsigned long)s_si_warm, (unsigned long)s_si_cyc_move,
             (unsigned long)s_si_cyc_settle, s_si_cyc_move / (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             s_si_cyc_settle / (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
}

/* after every learned gesture: the SYS-line summary, and NVS when the gate state changed or every 32 gestures */
static void si_publish(void)
{
    static const char *const ST[] = { "learning", "on", "off" };
    si_metrics_t m;
    char b[sizeof s_si_brief];
    si_metrics_get(&s_si, &m);
    snprintf(b, sizeof b, "%s,%u/%ums,rdy%u,n%u/%u", ST[m.state < 3 ? m.state : 0], m.p95_a_ms, m.p95_b_ms,
             (m.ready_a_pm + 5u) / 10u, m.n_a, m.n_b);
    portENTER_CRITICAL(&s_si_mux);
    memcpy(s_si_brief, b, sizeof b);
    portEXIT_CRITICAL(&s_si_mux);
    bool changed = m.state != s_si_state_saved;
    if (changed || ++s_si_n % 32 == 0) {
        size_t n = si_save(&s_si, s_si_blob, sizeof s_si_blob, s_si_tag);
        if (n) mosaico_si_save_request(s_si_blob, n);
        if (changed && s_si_state_saved != 0xFF) si_log("smart swipe decision");
        s_si_state_saved = m.state;
    }
}

static void si_settle_at(uint32_t now, int page)
{
    uint32_t c0 = esp_cpu_get_cycle_count();
    si_settle(&s_si, now, (uint8_t)page);
    uint32_t c = esp_cpu_get_cycle_count() - c0;
    if (c > s_si_cyc_settle) s_si_cyc_settle = c;
    s_si_lifted = false;
    s_si_wait = -1;
    si_publish();
}

/* from touch_read_cb, every touch read (LVGL task) */
static void si_touch(lv_indev_t *indev, bool pressed, int32_t x, int32_t y)
{
    static bool down;
    static int16_t lx, ly;
    if (!s_si_on) return;
    uint32_t now = si_now();
    if (pressed && !down) {
        down = true;
        if (s_si_lifted || s_si_wait >= 0) { si_abort(&s_si); s_si_lifted = false; s_si_wait = -1; }   /* touched mid-snap */
        s_si_gest = s_tv && s_cur < 0 && lv_screen_active() == s_home && !s_modal_open() && lv_obj_get_scroll_x(s_tv) % UI_W == 0;
        if (!s_si_gest) return;
        s_si_page0 = (uint8_t)lv_obj_get_index(lv_tileview_get_tile_active(s_tv));
        s_si_act = si_acts(&s_si);
        s_si_pred.dest = -1;
        si_begin(&s_si, now, (int16_t)x, (int16_t)y, s_si_page0);
    } else if (pressed) {
        if (!s_si_gest) return;
        uint32_t c0 = esp_cpu_get_cycle_count();
        si_pred_t p = si_move(&s_si, now, (int16_t)x, (int16_t)y);
        if (p.valid) {
            uint32_t c = esp_cpu_get_cycle_count() - c0;
            if (c > s_si_cyc_move) s_si_cyc_move = c;
            s_si_pred = p;
        }
    } else if (down) {
        down = false;
        if (!s_si_gest) return;
        s_si_gest = false;
        s_si_pred = si_end(&s_si, now, lx, ly, si_fresh_mask());
        if (lv_indev_get_scroll_obj(indev) == s_tv) s_si_lifted = true;   /* the snap follows: settle at its end */
        else si_settle_at(now, s_si_page0);                               /* a tap or a cross-axis drag: stays */
    }
    if (pressed) { lx = (int16_t)x; ly = (int16_t)y; }
}

/* LV_EVENT_SCROLL_THROW_BEGIN, sent by LVGL right after it sees the lift. Arm A: if the learner's page is where the throw
   lands, run LVGL's own throw handler now (it picks the snap point and starts the snap animation), one animation tick
   sooner than LVGL's throw animation would. The page is LVGL's either way. */
static void si_throw_cb(lv_event_t *e)
{
    lv_indev_t *in = (lv_indev_t *)lv_event_get_param(e);
    if (!in || !s_si_lifted || !s_si_act || s_si_pred.dest < 0) return;
    int32_t land = lv_obj_get_scroll_x(s_tv) - lv_indev_scroll_throw_predict(in, LV_DIR_HOR);
    int t = land <= 0 ? 0 : (int)((land + UI_W / 2) / UI_W);
    if (t < s_si_page0 - 1) t = s_si_page0 - 1;            /* the tileview scrolls one page per gesture */
    if (t > s_si_page0 + 1) t = s_si_page0 + 1;
    if (t >= s_ntiles) t = s_ntiles - 1;
    if (t != s_si_pred.dest) return;                       /* disagreement: LVGL snaps on its next tick, as before */
    s_si_early++;
    lv_indev_scroll_throw_handler(in);
}

/* LV_EVENT_SCROLL_END: the throw handler's carries the indev and comes just after it started the snap animation; the
   animation's own END carries nothing. A throw END with no animation running means the drag ended on a page already. */
static void si_scroll_end_cb(lv_event_t *e)
{
    if (!s_si_lifted || (lv_event_get_param(e) != NULL && lv_anim_get(s_tv, NULL))) return;
    int32_t sx = lv_obj_get_scroll_x(s_tv);
    if (sx % UI_W) { si_abort(&s_si); s_si_lifted = false; return; }   /* the snap was interrupted */
    int page = (int)(sx / UI_W);
    if (!s_blit_on || si_fresh(page)) si_settle_at(si_now(), page);   /* the panel already shows the right page */
    else { s_si_lifted = false; s_si_wait = page; }                    /* stale: settled when the redraw is shown */
}

static void si_render_ready(void) { if (s_si_wait >= 0) si_settle_at(si_now(), s_si_wait); }

/* from the 1 s Home refresh (tick_cb). Arm A: resting on a page from which the user usually swipes to Home, retake the
   Home picture when Home's content changed, so that slide starts from a current picture (about 120 ms of the LVGL task,
   never within 700 ms of a touch). */
static void si_warm_tick(void)
{
    uint8_t pct;
    if (!s_si_on || !s_tv || s_si_gest || s_sliding || !si_acts(&s_si) || s_cur >= 0 || lv_screen_active() != s_home) return;
    int32_t sx = lv_obj_get_scroll_x(s_tv);
    if (sx % UI_W || sx == 0 || lv_anim_get(s_tv, NULL) || lv_display_get_inactive_time(s_disp) < 700 || si_fresh(0)) return;
    if (si_hint_page(&s_si, (uint8_t)(sx / UI_W), &pct) != 0 || pct < 40) return;
    if (tile_snap(0)) s_si_warm++;
}

static void si_attach(lv_obj_t *tv)
{
    lv_obj_add_event_cb(tv, si_throw_cb, LV_EVENT_SCROLL_THROW_BEGIN, NULL);
    lv_obj_add_event_cb(tv, si_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    if (s_si_on && (s_si_gest || s_si_lifted || s_si_wait >= 0)) si_abort(&s_si);
    s_si_gest = s_si_lifted = false;
    s_si_wait = -1;
}

/* display_init, after the launcher is built: the model and the gate's verdict from NVS. A different firmware image
   (ELF SHA) keeps the model and re-runs the A/B test; so does a settings reset. */
static void si_boot(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    size_t n = sizeof s_si_blob;
    memcpy(&s_si_tag, d->app_elf_sha256, sizeof s_si_tag);
    si_init(&s_si, (uint8_t)s_ntiles, 0x5EED1E55u);
    bool loaded = mosaico_si_load(s_si_blob, &n) && si_load(&s_si, s_si_blob, n, s_si_tag);
    if (!s_set_loaded) si_retest(&s_si);
    s_si_on = true;
    si_log(loaded ? "smart swipe: model loaded" : "smart swipe: new model");
}

void ui_swipe_status(char *buf, size_t n) { si_metrics_line(&s_si, buf, n); }

void mosaico_display_swipe_brief(char *out, size_t n)
{
    if (!n) return;
    portENTER_CRITICAL(&s_si_mux);
    size_t k = strnlen(s_si_brief, sizeof s_si_brief - 1);
    if (k >= n) k = n - 1;
    memcpy(out, s_si_brief, k);
    out[k] = 0;
    portEXIT_CRITICAL(&s_si_mux);
    if (!k && n > 1) { out[0] = '-'; out[1] = 0; }         /* nothing learned yet */
}
#elif CONFIG_OCM_SWIPE_INTENT
#error "CONFIG_OCM_SWIPE_INTENT needs the snapshot slide (UI_SNAP_SLIDE and UI_BLIT_SLIDE)"
#endif

#if CONFIG_OCM_UI_METRICS
/* ---------- Smart UI metrics: one event per touch interaction, lift -> settled (ui_metrics.h has the definitions) ----------
   Press / lift come from touch_read_cb (the first read down / up, before LVGL processes it); the class comes from the
   object LVGL's indev hands to LV_EVENT_PRESSED (an app cell, a UI_FLAG_BACK control, an in-app control) or, at the
   lift, from the object the drag scrolled (the launcher's tileview = a swipe, anything else = a list). A tap on an app
   or Back counts only if LVGL then sends CLICKED (a press that slid off opened nothing). The settle is polled by a
   16 ms LVGL timer that runs only while an event waits for its settle. */
#include "src/misc/lv_event_private.h"                /* lv_event_dsc_t.filter: which events a control listens to */
#if CONFIG_OCM_UI_PREFETCH
#define UM_FAMILY ((1u << UM_APP) | (1u << UM_BACK))   /* classes with a prefetch action (arm A1) */
#else
#define UM_FAMILY 0u                                   /* no prefetch action: every event is A0 */
#endif
#define UM_RING 512
static um_t s_um;                                     /* 2.8 KB internal RAM: touched on every event */
EXT_RAM_BSS_ATTR static uint8_t s_um_blob[UM_BLOB_MAX];
static uint32_t s_um_since_save, s_um_tag;
static uint32_t s_um_cyc_ev, s_um_cyc_ev_sum, s_um_cyc_n, s_um_cyc_gate, s_um_interrupted, s_um_cancelled;
static lv_timer_t *s_um_timer;
static lv_indev_t *s_um_indev;
static bool s_um_on;
static struct {
    bool down, waiting, clicked, replay;
    uint8_t cls, arm, page0, fresh;
    int8_t si_dest;
    uint16_t target;
    uint32_t t_press, t_down, t_lift;
    lv_obj_t *obj;
} s_ue;

static uint32_t um_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* a small id for a control: the app on screen and its index path, folded to a byte */
static uint16_t um_obj_id(lv_obj_t *o)
{
    uint32_t h = 2166136261u;
    for (int d = 0; o && d < 6 && o != s_body && o != lv_layer_top(); d++, o = lv_obj_get_parent(o)) {
        h ^= (uint32_t)lv_obj_get_index(o) + 1u;
        h *= 16777619u;
    }
    return (uint16_t)(((unsigned)(s_cur & 0xFF) << 8) | (h & 0xFFu));
}

/* a control the user operates: a known input widget, or anything with a click / value handler */
static bool um_is_control(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_slider_class) || lv_obj_check_type(o, &lv_switch_class) || lv_obj_check_type(o, &lv_button_class) ||
        lv_obj_check_type(o, &lv_dropdown_class) || lv_obj_check_type(o, &lv_roller_class) || lv_obj_check_type(o, &lv_checkbox_class))
        return true;
    uint32_t n = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        uint32_t f = d ? d->filter & ~(uint32_t)LV_EVENT_PREPROCESS : 0;
        if (f == LV_EVENT_CLICKED || f == LV_EVENT_SHORT_CLICKED || f == LV_EVENT_VALUE_CHANGED || f == LV_EVENT_PRESSED ||
            f == LV_EVENT_RELEASED || f == LV_EVENT_LONG_PRESSED || f == LV_EVENT_ALL) return true;
    }
    return false;
}

static void um_poll_cb(lv_timer_t *t);
static void um_finish(uint32_t lat, uint8_t why);

static void um_wait_start(void)
{
    um_settle_lift(&s_ums, s_ue.t_lift);
    s_ue.waiting = true;
    lv_timer_reset(s_um_timer);
    lv_timer_resume(s_um_timer);
}

/* from touch_read_cb, every read (LVGL task) */
static void um_touch(lv_indev_t *indev, bool pressed)
{
    if (!s_um_on) return;
    uint32_t now = um_now();
    if (pressed) {
        if (!s_ue.down) {
            if (s_ue.waiting) {                          /* touched again before the page settled: the touch ends it */
                um_settle_abort(&s_ums);
                s_um_interrupted++;
                um_finish(now - s_ue.t_lift, UM_F_INTERRUPTED);
            }
#if CONFIG_OCM_UI_PREFETCH
            pf_act(upf_settle(&s_pf));                   /* a new touch: nothing stays built from the last one */
            pd_run();
#endif
            memset(&s_ue, 0, sizeof s_ue);
            s_ue.down = true;
            s_ue.t_press = now;
            s_ue.cls = UM_NONE;
            s_ue.si_dest = -1;
        }
        s_ue.t_down = now;
        return;
    }
    if (!s_ue.down) return;
    s_ue.down = false;
    s_ue.t_lift = now < s_ue.t_down + UM_READ_MS ? now : s_ue.t_down + UM_READ_MS;   /* see ui_metrics.h, "lift" */
    lv_obj_t *so = lv_indev_get_scroll_obj(indev);
    if (so && so == s_tv) {
        s_ue.cls = UM_SWIPE;
        s_ue.page0 = (uint8_t)lv_obj_get_index(lv_tileview_get_tile_active(s_tv));
        s_ue.arm = UM_A0;
#if CONFIG_OCM_SWIPE_INTENT
        s_ue.arm = s_si_act ? UM_A1 : UM_A0;             /* smart swipe's own A/B arm: it gates itself */
        s_ue.si_dest = s_si_pred.dest;
        s_ue.fresh = si_fresh_mask();
#endif
    } else if (so) {
#if CONFIG_OCM_UI_PREFETCH
        pf_act(upf_scroll_begin(&s_pf));
#endif
        s_ue.cls = UM_LIST;
        s_ue.target = um_obj_id(so);
        s_ue.arm = um_arm(&s_um, UM_LIST);
    }
    if (s_ue.cls == UM_NONE) return;
    um_wait_start();
}

/* LV_EVENT_PRESSED / LV_EVENT_CLICKED from the touch indev: param = the object */
static void um_indev_cb(lv_event_t *e)
{
    lv_obj_t *o = (lv_obj_t *)lv_event_get_param(e);
    if (!s_um_on || !o) return;
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) { if (o == s_ue.obj) s_ue.clicked = true; return; }
    if (!s_ue.down) return;
    lv_obj_t *scr = lv_obj_get_screen(o);
    s_ue.obj = o;
    if (scr == s_home && s_cur < 0) {
        for (lv_obj_t *p = o; p && p != s_home; p = lv_obj_get_parent(p))
            for (int i = 0; i < s_nicons; i++)
                if (s_icon_cell[i] == p) { s_ue.cls = UM_APP; s_ue.target = s_icon_app[i]; s_ue.obj = p; goto classed; }
        return;                                          /* a card or the dots: only a swipe is measured */
    }
    if (scr != s_appscr && scr != lv_layer_top()) return;
    for (lv_obj_t *p = o; p && p != scr; p = lv_obj_get_parent(p))
        if (lv_obj_has_flag(p, UI_FLAG_BACK)) {
            s_ue.cls = UM_BACK;
            s_ue.target = (uint16_t)(((unsigned)(s_cur & 0xFF) << 8) | (unsigned)(s_return_tile & 0xFF));
            s_ue.obj = p;
            goto classed;
        }
    for (lv_obj_t *p = o; p && p != scr && p != s_body; p = lv_obj_get_parent(p))
        if (um_is_control(p)) { s_ue.cls = UM_CONTROL; s_ue.target = um_obj_id(p); s_ue.obj = p; goto classed; }
    return;
classed:
    s_ue.arm = um_arm(&s_um, s_ue.cls);
#if CONFIG_OCM_UI_PREFETCH
    if (s_ue.arm == UM_A1 && (s_ue.cls == UM_APP || s_ue.cls == UM_BACK)) {
        lv_point_t pt = { 0, 0 };
        if (s_um_indev) lv_indev_get_point(s_um_indev, &pt);
        int pre = UPF_NONE;
        int a = upf_press(&s_pf, s_ue.cls == UM_APP ? UPF_K_APP : UPF_K_BACK, s_ue.target, um_now(), (int16_t)pt.x, (int16_t)pt.y, &pre);
        pf_act(pre);
        pf_act(a);
    }
#endif
}

static void um_frame(void)
{
    um_settle_frame(&s_ums, um_now());
}

static void um_save_now(void)
{
    size_t n = um_save(&s_um, s_um_blob, sizeof s_um_blob);
    if (n) mosaico_um_save_request(s_um_blob, n);
    s_um_since_save = 0;
}

static void um_finish(uint32_t lat, uint8_t why)
{
    s_ue.waiting = false;
    uint8_t c = s_ue.cls;
    bool ready = false;
#if CONFIG_OCM_UI_PREFETCH
    ready = (c == UM_APP || c == UM_BACK) && s_ue.arm == UM_A1 && upf_was_ready(&s_pf);
    pf_act(upf_settle(&s_pf));                           /* built but no click: thrown away */
    pd_run();                                            /* the app left by a prefetched Back is torn down now */
#endif
    if ((c == UM_APP || c == UM_BACK) && !s_ue.clicked) { s_um_cancelled++; um_dropped(&s_um, c); return; }
    if (c == UM_SWIPE && s_tv) {
        int fin = (int)(lv_obj_get_scroll_x(s_tv) / UI_W);
        s_ue.target = (uint16_t)((s_ue.page0 << 8) | (fin & 0xFF));
        ready = s_ue.si_dest >= 0 && s_ue.si_dest == fin && ((s_ue.fresh >> fin) & 1u);
    }
    uint32_t cr = esp_cpu_get_cycle_count();
    um_record(&s_um, c, s_ue.arm, lat, ready, why);      /* the module's per-event work */
    uint32_t c1 = esp_cpu_get_cycle_count(), ce = c1 - cr;
    if (ce > s_um_cyc_ev) s_um_cyc_ev = ce;
    s_um_cyc_ev_sum += ce;
    s_um_cyc_n++;
    if ((UM_FAMILY >> c) & 1u) {
        bool changed = false;
        if ((s_um.k[c].n[0] + s_um.k[c].n[1]) % 10u == 0) {   /* the gate looks every 10 samples of a gated class */
            changed = um_gate_step(&s_um);
            uint32_t cg = esp_cpu_get_cycle_count() - c1;
            if (cg > s_um_cyc_gate) s_um_cyc_gate = cg;
        }
        if (changed) {
            const um_verdict_t *v = &s_um.k[c].v;
            ESP_LOGW(TAG, "smart UI decision: %s %s (p95 %u -> %u ms, ready %u %%, z %.2f, n %u/%u)", um_class_name(c),
                     um_state_name(v->state), v->p95_0, v->p95_1, v->ready_pm / 10u, v->z100 / 100.0, v->n0, v->n1);
        }
        if (changed || ++s_um_since_save >= 32) um_save_now();
    }
}

static void um_poll_cb(lv_timer_t *t)
{
    if (!s_ue.waiting) { lv_timer_pause(t); return; }
    bool busy = s_ue.down || (s_um_indev && lv_indev_get_scroll_obj(s_um_indev)) || (s_tv && lv_anim_get(s_tv, NULL));
#if UI_BLIT_SLIDE
    busy = busy || s_blit_on;
#endif
    uint32_t lat = 0;
    int r = um_settle_poll(&s_ums, um_now(), busy, &lat);
    if (r == UM_S_WAIT) return;
    lv_timer_pause(t);
    if (r == UM_S_SETTLED || r == UM_S_TIMEOUT) um_finish(lat, r == UM_S_TIMEOUT ? UM_F_TIMEOUT : 0);
    else s_ue.waiting = false;
}

/* display_init: the gate's verdicts from NVS; a new firmware image (ELF SHA) re-tests every class */
static void um_boot(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    size_t n = sizeof s_um_blob;
    memcpy(&s_um_tag, d->app_elf_sha256, sizeof s_um_tag);
    um_init(&s_um, (uint32_t)esp_random() | 1u, UM_FAMILY, s_um_tag);
    bool loaded = mosaico_um_load(s_um_blob, &n) && um_load(&s_um, s_um_blob, n);
    s_um_indev = lv_indev_get_next(NULL);
    s_um_timer = lv_timer_create(um_poll_cb, UM_REFR_MS, NULL);
    lv_timer_pause(s_um_timer);
    s_um_on = true;
    ESP_LOGI(TAG, "smart UI metrics: %s, gated classes 0x%02x", loaded ? "gate state loaded" : "new gate state", (unsigned)UM_FAMILY);
}

#endif

#if CONFIG_OCM_UI_PREFETCH
/* ---------- Smart UI arm A1: press-time prefetch (ui_prefetch.h) ----------
   App icon, arm A1: once the finger rests for UPF_APP_DELAY_MS, the app is created in the spare app screen (its widgets
   in the PSRAM LVGL heap) and filled from a fresh snapshot; LVGL's click then shows that screen instead of building it.
   Back, arm A1: at the press the launcher behind the app is refreshed (an inactive screen: nothing is drawn); the
   click shows it at once and the app's widgets are deleted after the page has settled, not before the first frame.
   Nothing here changes where a touch lands: LVGL's own click picks the page, the prefetch only builds. */
#define PF_INT_FLOOR (48 * 1024)                       /* no prebuild below this much free internal RAM */
static uint32_t s_pf_build_us_max, s_pf_build_us_sum, s_pf_floor_skips;

static void pf_cancel_app(void)
{
    if (s_pf_app < 0) return;
    if (ui_app(s_pf_app)->destroy) ui_app(s_pf_app)->destroy();   /* no instance of it is live: the launcher is shown */
    lv_obj_clean(s_pf_body);
    s_pf_app = -1;
}

/* the app teardown a prefetched Back left for after the settle */
static void pd_run(void)
{
    if (s_pd_app < 0) return;
    if (ui_app(s_pd_app)->destroy) ui_app(s_pd_app)->destroy();
    lv_obj_clean(s_body);
    s_pd_app = -1;
}

static void pf_build(void)
{
    int64_t t0 = esp_timer_get_time();
    pd_run();
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < PF_INT_FLOOR) { s_pf_floor_skips++; s_pf.st = UPF_IDLE; return; }
    if (s_pf.kind == UPF_K_BACK) {
        if (s_cur < 0) { s_pf.st = UPF_IDLE; return; }
        take_snapshot();
        if (s_tv) lv_tileview_set_tile_by_index(s_tv, (uint32_t)(s_return_tile < s_ntiles ? s_return_tile : 0), 0, LV_ANIM_OFF);
        refresh_home(&s_snap);                            /* s_home is not the active screen: labels change, nothing draws */
        s_pf_home = true;
    } else {
        int idx = s_pf.target;
        if (s_cur >= 0 || idx >= ui_app_count()) { s_pf.st = UPF_IDLE; return; }
        uint8_t acc = s_accent_sec;
        s_accent_sec = ui_app(idx)->section;
        lv_obj_set_style_bg_grad_dir(s_pf_scr, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_bg_color(s_pf_scr, lv_color_hex(0x000000), 0);
        ui_app(idx)->create(s_pf_body);
        take_snapshot();
        ui_app(idx)->update(&s_snap);
        s_accent_sec = acc;
        s_pf_app = idx;
    }
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_pf_build_us_max) s_pf_build_us_max = us;
    s_pf_build_us_sum += us;
}

static void pf_act(int action)
{
    if (action == UPF_BUILD) pf_build();
    else if (action == UPF_CANCEL) { pf_cancel_app(); s_pf_home = false; }
}

/* LV_EVENT_PRESSING on an app cell, every touch read while the finger is on it */
static void pf_cell_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_um_indev || s_pf.st != UPF_ARMED) return;
    lv_point_t pt;
    lv_indev_get_point(s_um_indev, &pt);
    pf_act(upf_pressing(&s_pf, um_now(), (int16_t)pt.x, (int16_t)pt.y, lv_indev_get_scroll_obj(s_um_indev) != NULL));
}

/* icon_cb: true if the click opened the prebuilt app (the two app screens swap) */
static bool pf_open_app(int idx)
{
    int a = upf_click(&s_pf, UPF_K_APP, (uint16_t)idx);
    if (a != UPF_COMMIT || s_pf_app != idx) { pf_act(a == UPF_COMMIT ? UPF_CANCEL : a); return false; }
    lv_obj_t *scr = s_appscr, *body = s_body;
    s_appscr = s_pf_scr; s_body = s_pf_body;
    s_pf_scr = scr; s_pf_body = body;
    s_pf_app = -1;
    s_cur = idx;
    s_accent_sec = ui_app(idx)->section;
    refresh_status_bar(&s_snap);
    s_last_app_ms = s_snap.now_ms;
    lv_screen_load(s_appscr);
    ESP_LOGI(TAG, "app %s (built at the press)", ui_app(idx)->name);
    return true;
}

/* ui_go_home: true if a Back press already refreshed the launcher */
static bool pf_go_home(void)
{
    uint16_t target = (uint16_t)(((unsigned)(s_cur & 0xFF) << 8) | (unsigned)(s_return_tile & 0xFF));
    int a = upf_click(&s_pf, UPF_K_BACK, target);
    /* a Settings theme or order change still waiting for its debounce: the launcher refreshed at the press is stale,
       so the plain path runs (launcher_catch_up rebuilds it once) */
    bool stale = s_theme_deb.pending || s_launch_deb.pending;
    if (a != UPF_COMMIT || !s_pf_home || s_cur < 0 || stale) { s_pf_home = false; if (a == UPF_COMMIT) s_pf.st = UPF_IDLE; return false; }
    s_pf_home = false;
    s_pd_app = s_cur;                                     /* its widgets go after the settle (pd_run) */
    s_cur = -1;
    lv_screen_load(s_home);
    refresh_status_bar(&s_snap);
    return true;
}

#endif


/* ---------- boot splash: the 160 px mark on black until the launcher is up (at most 1.5 s, or the wallpaper is ready) ---- */
static lv_obj_t *s_splash;
static int64_t s_splash_until;

/* ---------- navigation ---------- */
static void close_app(void)
{
#if CONFIG_OCM_UI_PREFETCH
    pd_run();
#endif
    if (s_cur >= 0 && ui_app(s_cur)->destroy) ui_app(s_cur)->destroy();
    if (s_body) lv_obj_clean(s_body);
    s_cur = -1;
}

static void open_app(int idx)
{
    if (idx < 0 || idx >= ui_app_count()) return;
#if CONFIG_OCM_UI_PREFETCH
    pf_act(upf_settle(&s_pf));                            /* any other way into an app drops a prebuilt one */
#endif
    close_app();
    s_cur = idx;
    s_accent_sec = ui_app(idx)->section;
    lv_obj_set_style_bg_grad_dir(s_appscr, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_color(s_appscr, lv_color_hex(UI_BG), 0);      /* the palette's page ground (dark: pure black) */
    ui_app(idx)->create(s_body);
    take_snapshot();
    ui_app(idx)->update(&s_snap);
    refresh_status_bar(&s_snap);
    s_last_app_ms = s_snap.now_ms;
    if (lv_screen_active() != s_appscr) lv_screen_load(s_appscr);
    ESP_LOGI(TAG, "app %s", ui_app(idx)->name);
}

/* back from an app: the launcher page it was opened from */
static void launcher_catch_up(void);
void ui_go_home(void)
{
#if CONFIG_OCM_UI_PREFETCH
    if (pf_go_home()) return;                             /* the launcher was refreshed at the Back press */
#endif
    close_app();
    launcher_catch_up();
    if (lv_screen_active() != s_home) lv_screen_load(s_home);
    if (s_tv) lv_tileview_set_tile_by_index(s_tv, (uint32_t)(s_return_tile < s_ntiles ? s_return_tile : 0), 0, LV_ANIM_OFF);
    take_snapshot();
    refresh_status_bar(&s_snap);
    refresh_home(&s_snap);
}

static void go_home_tile(void) { s_return_tile = 0; ui_go_home(); }

static void step_app(int dir)
{
    int order[UI_MAX_APPS], n = home_order(order);
    if (!n) return;
    int pos = 0;
    for (int i = 0; i < n; i++) if (order[i] == s_cur) pos = i;
    open_app(order[(pos + dir + n) % n]);
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_LEFT) step_app(+1);
    else if (d == LV_DIR_RIGHT) step_app(-1);
    else if (d == LV_DIR_BOTTOM) ui_go_home();
}


/* ---------- theme: Auto = light by day once the clock is known (the S3 rule: config.json day_start_hour..day_end_hour,
   06:00-17:00 by default), dark at night and with no clock. Applied with one rebuild of every screen, never while a
   finger is on the glass or a question is open; checked once a second, so it follows the minute. ---------- */
static void rebuild_all(void);

/* Settings > Theme drives the launcher (Home, Apps), the status row and every app page (T1; v9 kept the apps dark):
   Light, Dark, or Auto = Light from config.json day_start_hour to day_end_hour (06:00-17:00 by default, the S3 rule) once
   the clock is known, Dark otherwise and with no clock */
static bool light_wanted(const app_status_t *st, bool *is_auto);
static const ui_theme_t *theme_wanted(const app_status_t *st, bool *is_auto)
{
    s_light = light_wanted(st, is_auto);
    return s_light ? &THEME_LIGHT : &THEME_DARK;
}
static bool s_theme_reopen;
bool ui_theme_reopening(void) { return s_theme_reopen; }

static bool light_wanted(const app_status_t *st, bool *is_auto)
{
    ui_settings_t set;
    ui_settings_get(&set);
    *is_auto = set.theme == 0;
    if (set.theme == 1) return true;
    if (set.theme == 2) return false;
    if (!mosaico_clock_known(st)) return false;
    const remote_config_t *rc = remote_config_get();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return rc->day_start_hour < rc->day_end_hour ? (tm.tm_hour >= rc->day_start_hour && tm.tm_hour < rc->day_end_hour)
                                                 : (tm.tm_hour >= rc->day_start_hour || tm.tm_hour < rc->day_end_hour);
}

static bool theme_tick(const ui_snap_t *s, bool force)
{
    bool is_auto;
    bool want = light_wanted(s->st, &is_auto);
    if (want == s_light) return false;
    if (!force && (lv_display_get_inactive_time(s_disp) < 1500 || s_modal_open())) return false;   /* not mid-gesture */
    s_light = want;
    ui_th = s_light ? &THEME_LIGHT : &THEME_DARK;
    int64_t tb = esp_timer_get_time();
    /* one rebuild in the new palette: the status row and its banner, the launcher (both wallpapers are already in
       PSRAM) and the app on screen; the Power sheet closes */
    if (s_psheet) { lv_obj_delete(s_psheet); s_psheet = NULL; }
    if (s_sb) lv_obj_delete(s_sb);
    if (s_banner) lv_obj_delete(s_banner);
    build_status_bar();
    build_launcher();
    int64_t ta = esp_timer_get_time();
    lv_obj_set_style_bg_color(s_appscr, lv_color_hex(UI_BG), 0);
    if (s_cur >= 0) { s_theme_reopen = true; open_app(s_cur); s_theme_reopen = false; }
    ESP_LOGI(TAG, "theme -> %s (%s, %s), launcher rebuilt in %.1f ms, app in %.1f ms", s_light ? "light" : "dark", is_auto ? "auto" : "set",
             s->clock, (ta - tb) / 1000.0, (esp_timer_get_time() - ta) / 1000.0);
    return true;
}

/* Back to the launcher while a theme or order change still waits for its debounce: do it now, one rebuild at most */
static void launcher_catch_up(void)
{
    bool theme = s_theme_deb.pending, order = s_launch_deb.pending;
    ocm_debounce_cancel(&s_theme_deb);
    ocm_debounce_cancel(&s_launch_deb);
    if (theme) {
        take_snapshot();
        if (theme_tick(&s_snap, true)) { s_theme_check = false; return; }
    }
    if (order) build_launcher();
}

/* ---------- panel power ---------- */
static void panel_power(bool on)
{
    if (on == s_panel_on) return;
    if (on) {
        if (s_panel_slpin) esp_lcd_panel_disp_sleep(s_panel, false);
        esp_lcd_panel_disp_on_off(s_panel, true);
        ui_settings_t set;
        ui_settings_get(&set);
        esp_lcd_panel_co5300_set_brightness(s_panel, set.brightness);
        lv_obj_invalidate(lv_screen_active());
    } else {
        esp_lcd_panel_co5300_set_brightness(s_panel, 0);
        esp_lcd_panel_disp_on_off(s_panel, false);
        if (s_panel_slpin) esp_lcd_panel_disp_sleep(s_panel, true);
    }
    s_panel_on = on;
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off");
}

/* ---------- the periodic UI job (LVGL timer, 50 ms) ---------- */
static void slide_tick(void);
/* UI stall guard: if the LVGL timer has not run the UI tick for 15 s (it never subscribes to the TWDT when the very first
   tick never comes), abort from the esp_timer task so the core dump holds every task's backtrace. The flash FAT is
   quiesced first (no sector left half written); the core dump goes to its own partition. */
#if CONFIG_OCM_STORAGE_RESET_TEST
#define STALL_ARM_US   8000000                         /* the reset-injection test's stall variant: shorter cycles */
#define STALL_LIMIT_US 3000000
#else
#define STALL_ARM_US   20000000
#define STALL_LIMIT_US 15000000
#endif
static volatile int64_t s_tick_seen_us;
static void stall_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now > STALL_ARM_US && now - s_tick_seen_us > STALL_LIMIT_US) {
        ESP_LOGE(TAG, "ui tick stalled for %lld s: aborting for a core dump", (long long)((now - s_tick_seen_us) / 1000000));
        sdlog_quiesce_for_reset();
        esp_system_abort("ui tick stalled");
    }
}
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    static int64_t next_us;
    int64_t t0 = esp_timer_get_time();
    s_tick_seen_us = t0;
    if (s_job < 0) s_job = sched_register("ui", UI_PERIOD_MS, UI_PERIOD_MS, false);
    if (next_us) {
        int64_t late_ms = (t0 - next_us) / 1000;
        if (late_ms > (int64_t)s_late_max_ms) s_late_max_ms = (uint32_t)late_ms;
        next_us += UI_PERIOD_MS * 1000;
        if (t0 - next_us > 1000000) next_us = t0;          /* after a long stall, start the cadence again */
    } else next_us = t0 + UI_PERIOD_MS * 1000;

    /* Page pictures (tile_snap, 90-130 ms of CPU each, and a slide that the Function key starts from here, whose
       SCROLL_BEGIN takes the Home picture) are aperiodic work and run outside the "ui" job, like home_pic_cb. The job
       measures only the periodic screen work. Measured on the test firmware (2026-11-11, slides started from the tick,
       back to back): with the pictures inside, 46 of 191 slides gave ui runs of 100-875 ms (a flash FAT sync stalls the
       LVGL task while the cache is off) and sched_mon's 3x-deadline limit (150 ms) restarted the board; with them outside,
       0 of 269 runs passed 100 ms (worst 63 ms). */
    slide_tick();
    static bool s_first_tick;
    if (!s_first_tick) { s_first_tick = true; ESP_LOGI(TAG, "ui: first tick"); esp_task_wdt_add(NULL); }
    esp_task_wdt_reset();                                 /* the LVGL task is watched: a hang becomes a reset with a core dump */
    if (s_wall_ready && s_wall_img && lv_image_get_src(s_wall_img) != &s_wall_dsc2[s_light]) {
        lv_image_set_src(s_wall_img, &s_wall_dsc2[s_light]);
        ESP_LOGI(TAG, "wallpapers ready: dark and light computed in %lu ms (PSRAM %u KB)", (unsigned long)s_wall_ms, (unsigned)(2 * UI_W * UI_H * 2 / 1024));
    }
    if (s_splash && ((s_wall_ready && t0 / 1000 > s_splash_until - 1000) || t0 / 1000 > s_splash_until)) {
        lv_screen_load(s_home);
        lv_obj_delete(s_splash);
        s_splash = NULL;
    }
    if (s_req_wake) { s_req_wake = false; lv_display_trigger_activity(s_disp); panel_power(true); }
    if (s_req_home) {
        s_req_home = false;
        lv_display_trigger_activity(s_disp);
        if (!s_panel_on) panel_power(true);
        else go_home_tile();
    }
    if (s_req_off) { s_req_off = false; panel_power(false); }
    /* a theme change from Settings: one check (and at most one launcher rebuild) once the taps stopped */
    if (s_theme_deb.pending && lv_display_get_inactive_time(s_disp) >= OCM_IDLE_FINGER_MS &&
        ocm_debounce_fire(&s_theme_deb, t0 / 1000, INT32_MAX)) s_theme_check = true;

    int64_t tj = esp_timer_get_time();
    sched_begin(s_job);
    if (s_req_bright >= 0) {                              /* at most one brightness command per tick (50 ms) */
        if (s_panel_on) { esp_lcd_panel_co5300_set_brightness(s_panel, (uint8_t)s_req_bright); s_bright_cmds++; }
        s_req_bright = -1;
    }

    ui_settings_t set;
    ui_settings_get(&set);
    uint32_t idle_ms = lv_display_get_inactive_time(s_disp);
    int power_to = -1;                                    /* the panel's own commands run after the job (see below) */
    if (s_panel_on && set.screen_off_s && idle_ms > set.screen_off_s * 1000u) power_to = 0;
    else if (!s_panel_on && idle_ms < 200) power_to = 1;   /* a touch wakes it */

    bool sec_due = false;
    if (s_panel_on) {
        int64_t now = t0 / 1000;
        if (now - s_last_sec_ms >= 1000 || s_theme_check) {
            s_last_sec_ms = now;
            take_snapshot();
            if (s_snap.gauge.ok && !(s_snap.gauge.ma > -5 && s_snap.gauge.ma < 5)) {
                if (s_pw_n == 60) { memmove(s_pw_hist, s_pw_hist + 1, 59 * sizeof(float)); s_pw_n = 59; }
                s_pw_hist[s_pw_n++] = s_snap.gauge.mv * fabsf((float)s_snap.gauge.ma) / 1e6f;
            } else s_pw_n = 0;
            sec_due = true;
            refresh_status_bar(&s_snap);
            if (s_cur < 0 && lv_screen_active() == s_home) refresh_home(&s_snap);
        }
        if (mosaico_drive_take_auto_open())                  /* after End drive: the drive summary */
            for (int i = 0; i < ui_app_count(); i++) if (!strcmp(ui_app(i)->id, "summary")) { open_app(i); break; }
        if (s_cur >= 0 && now - s_last_app_ms >= ui_app(s_cur)->period_ms) {
            if (now - s_last_snap_ms >= 200) take_snapshot();
            ui_app(s_cur)->update(&s_snap);
            s_last_app_ms = now;
        }
    }

    static int64_t s_rlog_us;
    if (t0 - s_rlog_us >= 10LL * 1000000) {
        s_rlog_us = t0;
        ESP_LOGI(TAG, "ui alive: %lu ticks, screen %s, splash %d, wall %d", (unsigned long)s_runs,
                 lv_screen_active() == s_home ? "home" : lv_screen_active() == s_appscr ? "app" : "other", s_splash != NULL, (int)s_wall_ready);
        {
            static sched_stats_t js[12];
            int nj = sched_get_stats(js, 12);
            for (int i = 0; i < nj; i++)
                if (js[i].misses) ESP_LOGI(TAG, "deadline: job %s missed %lu time(s), worst run %.1f ms against a %lu ms deadline", js[i].name,
                                           (unsigned long)js[i].misses, js[i].max_us / 1000.0, (unsigned long)js[i].deadline_ms);
        }
        if (s_render_max_us)
            ESP_LOGI(TAG, "render: worst %.1f ms in 10 s, theme %s, ui misses %lu, internal %u KB, PSRAM %u KB free", s_render_max_us / 1000.0,
                     ui_th->name, (unsigned long)s_misses, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        s_render_max_us = 0;
    }
    sched_end(s_job);
    uint32_t us = (uint32_t)(esp_timer_get_time() - tj);
    s_runs++;
    if (us > s_max_us) s_max_us = us;
    if (us > UI_PERIOD_MS * 1000u) s_misses++;

    /* screen off / on by the idle timer and by a touch: the CO5300 commands are aperiodic work and run outside the
       "ui" job, like the Function key's wake above (the panel stays dark until it is on: nothing is drawn while off) */
    if (power_to >= 0) panel_power(power_to == 1);

    /* once a second, after the job: a theme change rebuilds the launcher; smart swipe may retake the Home picture */
    if (sec_due) {
        bool force = s_theme_check;
        s_theme_check = false;
        if (theme_tick(&s_snap, force)) {
            ocm_debounce_cancel(&s_launch_deb);          /* the rebuild carries a new Home order too */
            refresh_status_bar(&s_snap);
            if (s_cur < 0 && lv_screen_active() == s_home) refresh_home(&s_snap);
        }
#if CONFIG_OCM_SWIPE_INTENT
        si_warm_tick();
#endif
    }
    /* Home order moved in Settings: one launcher rebuild once the moves stopped, outside the job, finger up (the
       launcher is off screen; its pictures follow on the next launcher visit) */
    if (s_launch_deb.pending && lv_display_get_inactive_time(s_disp) >= OCM_IDLE_FINGER_MS &&
        ocm_debounce_fire(&s_launch_deb, esp_timer_get_time() / 1000, INT32_MAX)) {
        int64_t tb = esp_timer_get_time();
        build_launcher();
        ESP_LOGI(TAG, "launcher rebuilt for the new Home order in %.1f ms", (esp_timer_get_time() - tb) / 1000.0);
    }
}

/* ---------- touch ----------
   Own LVGL read callback instead of lvgl_port_add_touch: esp_lvgl_port wraps the I2C read in ESP_ERROR_CHECK, and one
   failed read (seen ~2 s after the BLE controller starts, 2026-11-11 core dump: abort in lvgl_port_touchpad_read)
   aborts the whole firmware. Here a failed read counts as "released" and is counted (Board app). */
static esp_lcd_touch_handle_t s_tp;
static uint32_t s_touch_err;
uint32_t mosaico_touch_errors(void) { return s_touch_err; }

/* CST9220 wedge recovery. Seen 2026-11-11 (once in hours of use, test firmware): one I2C write of the report address
   NACKed, and from then on every report read returned 0xff (driver: "Malformed report header") with more NACKs: no
   touch until a reset, and ~50 driver warnings a second from this task. The board has no TP_RST line (BOARD_TOUCH_RST
   is NC), so the recovery is in software: after TOUCH_BAD_MAX consecutive failed or malformed reads the controller and
   its I2C device are opened again (the driver's wake + report-page sequence), at most once per TOUCH_RETRY_US; while
   failing the panel is read only every TOUCH_PROBE_US, and the drivers' warnings are muted after the first few, with
   one summary line per 10 s here instead. A legitimate ACK-only report is a single miss and resets nothing. */
#define TOUCH_BAD_MAX   30                             /* about 0.5 s of reads */
#define TOUCH_MUTE_AT   5
#define TOUCH_RETRY_US  2000000
#define TOUCH_PROBE_US  100000
static esp_lcd_panel_io_handle_t s_tp_io;
static uint32_t s_touch_bad, s_touch_bad_burst;
static uint32_t s_touch_reopens;
static int64_t s_touch_retry_us, s_touch_probe_us, s_touch_log_us;
static bool s_touch_muted;

static bool touch_open(void)
{
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9220_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(mosaico_i2c_bus(), &io_cfg, &s_tp_io) != ESP_OK) { s_tp_io = NULL; return false; }
    const esp_lcd_touch_config_t cfg = {
        .x_max = BOARD_LCD_H_RES, .y_max = BOARD_LCD_V_RES, .rst_gpio_num = BOARD_TOUCH_RST, .int_gpio_num = BOARD_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    if (esp_lcd_touch_new_i2c_cst9220(s_tp_io, &cfg, &s_tp) != ESP_OK) {
        s_tp = NULL;
        esp_lcd_panel_io_del(s_tp_io);
        s_tp_io = NULL;
        return false;
    }
    return true;
}

static void touch_close(void)
{
    if (s_tp) esp_lcd_touch_del(s_tp);
    s_tp = NULL;
    if (s_tp_io) esp_lcd_panel_io_del(s_tp_io);
    s_tp_io = NULL;
}

static void touch_mute(bool on)
{
    esp_log_level_t l = on ? ESP_LOG_NONE : ESP_LOG_INFO;
    esp_log_level_set("cst9220", l);
    esp_log_level_set("lcd_panel.io.i2c", l);
    s_touch_muted = on;
}

static void touch_good(void)
{
    if (s_touch_bad >= TOUCH_BAD_MAX)
        ESP_LOGW(TAG, "touch: reading again after %lu failed or malformed reads (controller re-opened %lu time%s so far)",
                 (unsigned long)s_touch_bad, (unsigned long)s_touch_reopens, s_touch_reopens == 1 ? "" : "s");
    if (s_touch_muted) touch_mute(false);
    s_touch_bad = 0;
}

static void touch_failed(int64_t now)
{
    s_touch_bad++;
    s_touch_bad_burst++;
    if (s_touch_bad == TOUCH_MUTE_AT) touch_mute(true);
    if (s_touch_bad >= TOUCH_BAD_MAX) {
        s_touch_probe_us = now + TOUCH_PROBE_US;
        if (now >= s_touch_retry_us) {
            s_touch_retry_us = now + TOUCH_RETRY_US;
            touch_close();
            bool ok = touch_open();
            s_touch_reopens++;
            ESP_LOGW(TAG, "touch: %lu failed or malformed reads in a row: controller re-opened (%s)", (unsigned long)s_touch_bad,
                     ok ? "ok" : "failed, retrying");
        }
    }
    if (s_touch_muted && now - s_touch_log_us >= 10000000) {
        s_touch_log_us = now;
        ESP_LOGW(TAG, "touch: %lu failed or malformed reads in the last 10 s (driver warnings muted)", (unsigned long)s_touch_bad_burst);
        s_touch_bad_burst = 0;
    }
}

/* one panel read with the wedge recovery; ESP_OK = the driver holds a fresh report */
static esp_err_t touch_read(void)
{
    int64_t now = esp_timer_get_time();
    if (s_touch_bad >= TOUCH_BAD_MAX && now < s_touch_probe_us) return ESP_ERR_INVALID_STATE;   /* failing: next probe */
    esp_err_t e = s_tp ? esp_lcd_touch_read_data(s_tp) : ESP_ERR_INVALID_STATE;
    if (e == ESP_OK) touch_good(); else touch_failed(now);
    return e;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t n = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    if (touch_read() != ESP_OK) {
        s_touch_err++;
#if CONFIG_OCM_SWIPE_INTENT
        si_touch(indev, false, 0, 0);                    /* LVGL sees a release: so does the learner */
#endif
#if CONFIG_OCM_UI_METRICS
        um_touch(indev, false);
#endif
        return;
    }
    if (esp_lcd_touch_get_data(s_tp, pt, &n, 1) == ESP_OK && n > 0) {
        data->point.x = pt[0].x;
        data->point.y = pt[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
#if CONFIG_OCM_SWIPE_INTENT
    si_touch(indev, data->state == LV_INDEV_STATE_PRESSED, data->point.x, data->point.y);
#endif
#if CONFIG_OCM_UI_METRICS
    um_touch(indev, data->state == LV_INDEV_STATE_PRESSED);
#endif
}

static void touch_init(void)
{
    if (!mosaico_i2c_bus()) return;
    if (!touch_open()) { ESP_LOGE(TAG, "CST9220 not found: no touch"); return; }
    lvgl_port_lock(0);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(in, s_disp);
    lv_indev_set_read_cb(in, touch_read_cb);
#if CONFIG_OCM_UI_METRICS
    lv_indev_add_event_cb(in, um_indev_cb, LV_EVENT_PRESSED, NULL);
    lv_indev_add_event_cb(in, um_indev_cb, LV_EVENT_CLICKED, NULL);
#endif
    lvgl_port_unlock();
    ESP_LOGI(TAG, "touch ready (CST9220)");
}

/* The CO5300 takes window coordinates in pairs: even start, odd end (Espressif's AMOLED examples), or partial
   updates leave a one-pixel seam. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    UM_INVALIDATED();
    a->x1 &= ~1; a->y1 &= ~1;
    a->x2 |= 1;  a->y2 |= 1;
}

/* one rebuild of everything in the palette in force: status bar, home, grid and the app on screen */
static void rebuild_all(void)
{
#if CONFIG_OCM_UI_PREFETCH
    pf_act(upf_settle(&s_pf));
    pd_run();
#endif
    int app = s_cur;
    if (s_tv && app < 0) s_return_tile = (int)lv_obj_get_index(lv_tileview_get_tile_active(s_tv));
    close_app();
    lv_obj_clean(lv_layer_top());
    s_modal = NULL;
    build_status_bar();
    build_launcher();
    lv_obj_set_style_bg_color(s_appscr, lv_color_hex(UI_BG), 0);
#if CONFIG_OCM_UI_PREFETCH
    lv_obj_set_style_bg_color(s_pf_scr, lv_color_hex(UI_BG), 0);
#endif
    if (app >= 0) open_app(app);
    else { take_snapshot(); refresh_status_bar(&s_snap); refresh_home(&s_snap); }
}


static void render_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) { s_rs_us = esp_timer_get_time(); UM_RENDER_START(); }
    else if (s_rs_us) {
        UM_FRAME();
        int64_t now = esp_timer_get_time();
        uint32_t d = (uint32_t)(now - s_rs_us);
        if (d > s_render_max_us) s_render_max_us = d;
#if CONFIG_OCM_SWIPE_INTENT
        si_render_ready();
#endif
    }
}


static lv_obj_t *make_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

esp_err_t display_init(void)
{
    /* the 3.3 V rail is already on (mosaico_board_init) */
/* swipe step d: draw buffer height in lines (two buffers, internal DMA RAM); the SPI transfer limit follows it */
#define UI_BUF_LINES 40
    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(BOARD_LCD_SCLK, BOARD_LCD_D0, BOARD_LCD_D1, BOARD_LCD_D2,
                                                              BOARD_LCD_D3, BOARD_LCD_H_RES * UI_BUF_LINES * sizeof(uint16_t));
    esp_err_t e = spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) return e;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(BOARD_LCD_CS, NULL, NULL);
    io_cfg.flags.psram_dma_direct = 1;                   /* PSRAM colour buffers (the slide pictures) go to DMA without a bounce copy */
    if ((e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io)) != ESP_OK) return e;
    s_io = io;
    co5300_vendor_config_t vendor = { .flags = { .use_qspi_interface = 1 } };
    const esp_lcd_panel_dev_config_t pcfg = { .reset_gpio_num = BOARD_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                              .bits_per_pixel = 16, .vendor_config = &vendor };
    if ((e = esp_lcd_new_panel_co5300(io, &pcfg, &s_panel)) != ESP_OK) return e;
    esp_lcd_panel_reset(s_panel);
    if ((e = esp_lcd_panel_init(s_panel)) != ESP_OK) return e;

    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 2;                               /* below the OBD poll (5) and the model (3): see the table */
    port.task_stack = 8192;
    port.task_affinity = 0;                               /* the OBD poll and the model run on core 1 */
    if ((e = lvgl_port_init(&port)) != ESP_OK) return e;
    const lvgl_port_display_cfg_t dcfg = {
        .io_handle = io, .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * UI_BUF_LINES, .double_buffer = true,
        .hres = BOARD_LCD_H_RES, .vres = BOARD_LCD_V_RES, .monochrome = false, .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&dcfg);
    if (!s_disp) return ESP_FAIL;
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(s_disp, render_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, render_cb, LV_EVENT_RENDER_READY, NULL);
    touch_init();

    settings_load();
    lvgl_port_lock(0);
    s_home = make_screen();
    s_appscr = make_screen();
    lv_obj_add_event_cb(s_appscr, gesture_cb, LV_EVENT_GESTURE, NULL);
    s_body = lv_obj_create(s_appscr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, UI_W, UI_H);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_GESTURE_BUBBLE);
#if CONFIG_OCM_UI_PREFETCH
    s_pf_scr = make_screen();                             /* the same as s_appscr: the two swap when a prebuilt app opens */
    lv_obj_add_event_cb(s_pf_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    s_pf_body = lv_obj_create(s_pf_scr);
    lv_obj_remove_style_all(s_pf_body);
    lv_obj_set_size(s_pf_body, UI_W, UI_H);
    lv_obj_remove_flag(s_pf_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_pf_body, LV_OBJ_FLAG_GESTURE_BUBBLE);
#endif
    take_snapshot();
    bool is_auto;
    ui_th = theme_wanted(s_snap.st, &is_auto);
    rebuild_all();
#if CONFIG_OCM_SWIPE_INTENT
    si_boot();
#endif
#if CONFIG_OCM_UI_METRICS
    um_boot();
#endif
    s_splash = make_screen();
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(0x000000), 0);
    lv_obj_t *sl = lv_image_create(s_splash);
    lv_image_set_src(sl, &ocm_logo_160);
    lv_obj_center(sl);
    lv_screen_load(s_splash);
    s_splash_until = esp_timer_get_time() / 1000 + 1500;
    xTaskCreatePinnedToCore(wall_task, "wallpaper", 3072, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "theme -> %s (%s, %s)", ui_th->name, is_auto ? "auto" : "set", s_snap.clock);
    lv_timer_create(tick_cb, UI_PERIOD_MS, NULL);
    lvgl_port_unlock();
    static esp_timer_handle_t stall_t;
    const esp_timer_create_args_t sa = { .callback = stall_cb, .name = "ui_stall" };
    if (esp_timer_create(&sa, &stall_t) == ESP_OK) esp_timer_start_periodic(stall_t, 1000000);

    /* under the LVGL lock: the LVGL task is already flushing the splash through the same SPI device, and two tasks'
       polling transactions on one device make spi_device_acquire_bus fail (spi_master.c: "Cannot acquire bus when a
       polling transaction is in progress"); that left a boot without its flush-done and the UI stalled */
    lvgl_port_lock(0);
    esp_lcd_panel_disp_on_off(s_panel, true);
    esp_lcd_panel_co5300_set_brightness(s_panel, s_set.brightness);
    s_panel_on = true;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "dashboard up: launcher (home + %d grid page%s), %d apps, 480x480 CO5300", s_ntiles - 1, s_ntiles == 2 ? "" : "s", ui_app_count());
    return ESP_OK;
}

/* ---------- display.h for the rest of the firmware (any task: flags only) ---------- */
void display_next_page(void) { s_req_home = true; }          /* Function key: home (wakes a dark screen first) */
void display_sleep(void) { s_req_off = true; }
int  display_current_page(void) { return s_cur + 1; }
void display_set_backlight(uint8_t pct) { if (pct == 0) s_req_off = true; else { s_req_bright = pct; s_req_wake = true; } }
void display_fade_off(int ms) { s_req_off = true; vTaskDelay(pdMS_TO_TICKS(ms > 100 ? ms : 100)); }
