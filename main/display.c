/* 1.47" ST7789 dashboard (portrait 172x320) rendered with LVGL 9 through esp_lvgl_port.
 * Page 0: live dashboard. Page 1: vehicle / adapter / clock and SD details. BOOT button cycles pages. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "remote_config.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "board.h"
#include "display.h"
#include "status.h"
#include "sdlog.h"
#include "power_state.h"
#include "econ.h"
#include "uploader.h"
#include "sys_mode.h"
#include "version.h"
#include <stdarg.h>

/* Change-only widget setters. LVGL 9 invalidates (and so redraws and re-sends over SPI) on every set, even when the
   value is the same, and the refresh timer re-applies every value 10 times a second: measured, that kept taskLVGL at
   50 % of a core on a static screen. These compare first and touch the widget only when the value differs. The
   macros below route every call in this file through them; the helpers themselves call the real LVGL functions. */
static void dc_set_text(lv_obj_t *o, const char *t)
{
    const char *cur = lv_label_get_text(o);
    if (t && cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(o, t);
}
static void dc_set_text_fmt(lv_obj_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dc_set_text_fmt(lv_obj_t *o, const char *fmt, ...)
{
    EXT_RAM_BSS_ATTR static char buf[768];           /* LVGL task only, so one static buffer is safe */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dc_set_text(o, buf);
}
#define DC_SAME_COLOR(get, o, c, sel) (lv_color_to_u32(get(o, sel)) == lv_color_to_u32(c))
static void dc_text_color(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel)
{ if (!DC_SAME_COLOR(lv_obj_get_style_text_color, o, c, sel)) lv_obj_set_style_text_color(o, c, sel); }
static void dc_bg_color(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel)
{ if (!DC_SAME_COLOR(lv_obj_get_style_bg_color, o, c, sel)) lv_obj_set_style_bg_color(o, c, sel); }
static void dc_border_color(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel)
{ if (!DC_SAME_COLOR(lv_obj_get_style_border_color, o, c, sel)) lv_obj_set_style_border_color(o, c, sel); }
static void dc_arc_color(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel)
{ if (!DC_SAME_COLOR(lv_obj_get_style_arc_color, o, c, sel)) lv_obj_set_style_arc_color(o, c, sel); }
static void dc_bg_opa(lv_obj_t *o, lv_opa_t v, lv_style_selector_t sel)
{ if (lv_obj_get_style_bg_opa(o, sel) != v) lv_obj_set_style_bg_opa(o, v, sel); }
static void dc_add_flag(lv_obj_t *o, lv_obj_flag_t f) { if (!lv_obj_has_flag(o, f)) lv_obj_add_flag(o, f); }
static void dc_remove_flag(lv_obj_t *o, lv_obj_flag_t f) { if (lv_obj_has_flag_any(o, f)) lv_obj_remove_flag(o, f); }
#define lv_label_set_text            dc_set_text
#define lv_label_set_text_fmt        dc_set_text_fmt
#define lv_obj_set_style_text_color  dc_text_color
#define lv_obj_set_style_bg_color    dc_bg_color
#define lv_obj_set_style_border_color dc_border_color
#define lv_obj_set_style_arc_color   dc_arc_color
#define lv_obj_set_style_bg_opa      dc_bg_opa
#define lv_obj_add_flag              dc_add_flag
#define lv_obj_remove_flag           dc_remove_flag
#include "ml_infer.h"
#include "ml_params.h"
#include "sched_mon.h"
#include "rev_task.h"
#include "esp_timer.h"

static const char *TAG = "display";

/* palette for a backlit IPS LCD, switched by time of day (light 06:00-17:00 by default) or by remote config.
 * An LCD's black is backlight leaking through, so the dark theme uses pure neutral black: any tint (the old navy) glows
 * as a blue field. Foregrounds are light and fairly unsaturated, and blue is never used for small text on black
 * (short-wavelength text is the hardest to focus). Status colours follow ISO 2575 tell-tales: red danger, yellow and
 * amber caution, green normal, and every status also carries a word. Text is at least 4.5:1 on bg and tile in both
 * themes, dim (an idle link) at least 3:1. Dark tiles are near-black (#050505): a grey fill reads as a lit panel
 * on this backlight, so tiles are drawn by their borders. Page 5 gives each of its seven sections its own colour,
 * placed by enumerating all arrangements: in dark, no two sections closer than 15.4 (OKLab x100) for normal vision and
 * every pair that touches at least 8.4 for colour-blind readers. Light uses the same hues deepened to saturated mid tones
 * on pure white (bg #FFFFFF, tiles #FCFCFC, the same on the RGB565 panel, so tiles are drawn by their borders as in
 * dark), brand navy text, a light blue accent, light warm oranges, no near-black tints that read as mud on the LCD:
 * body colours at least 4.5:1 on the white tile, the accent and the two oranges at least 3:1 (large numbers, bars, bold heads). The dark theme's pastels
 * would be 1.4 to 2.6:1 on white. Each section has a caption, so colour is never the only cue. All values stay
 * distinct after RGB565. */
typedef struct { uint32_t bg, tile, text, muted, accent, ok, warn, bad, border, dim, violet, blue, amber, sev_high, teal, pink,
                 orange, red, sage, power; } theme_t;
static const theme_t THEME_DARK  = {0x000000, 0x050505, 0xF2ECE1, 0xB8B8B8, 0x8CC8FF, 0x56F487, 0xFCCA21, 0xE64343, 0x4A4A4A, 0x7A7A7A,
                                    0xAD8DFD, 0x38B8F6, 0xF2A13A, 0xF08E54, 0x72ECE4, 0xFE86C2, 0xFE860F, 0xFD122E, 0x6CB35D, 0xF2A33A};
static const theme_t THEME_LIGHT = {0xFFFFFF, 0xFCFCFC, 0x15202D, 0x4A5563, 0x3B82F6, 0x15803D, 0xB45309, 0xC62828, 0x8C8C8C, 0x6B7280,
                                    0x7C3AED, 0x1D4ED8, 0xDD6E0E, 0xC2410C, 0x0E7490, 0xA21CAF, 0xEA6A12, 0xBE123C, 0x4D7C0F, 0xE5484D};
static const theme_t *T = &THEME_DARK;
#define C_BG      (T->bg)
#define C_TILE    (T->tile)
#define C_TEXT    (T->text)
#define C_MUTED   (T->muted)
#define C_ACCENT  (T->accent)
#define C_OK      (T->ok)
#define C_WARN    (T->warn)
#define C_BAD     (T->bad)
#define C_BORDER  (T->border)
#define C_DIM     (T->dim)
#define C_VIOLET  (T->violet)
#define C_BLUE    (T->blue)
#define C_TEAL    (T->teal)
#define C_PINK    (T->pink)
#define C_ORANGE  (T->orange)
#define C_RED     (T->red)
#define C_SAGE    (T->sage)
#define C_AMBER   (T->amber)      /* an estimated value (no PID behind it) */
#define C_POWER   (T->power)      /* the power bar on page 1: amber in dark, coral red next to the blue CPU bar in light */
#define C_SEV_HIGH (T->sev_high)  /* high-severity fault: orange, ISO 2575 caution, between C_WARN and C_BAD */

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static volatile int s_page;
static int s_shown_page = -1;

/* ---- widgets, page 0 ---- */
static lv_obj_t *scr0, *bar_top, *lbl_ble, *lbl_sd, *lbl_wifi, *lbl_clock;
static lv_obj_t *off_panel, *off_title, *off_text;   /* power-off overlay */
static lv_obj_t *lbl_clock_bold;   /* the same text 1 px to the right: faux bold, the fonts have no bold weight */
static lv_obj_t *lbl_rpm, *bar_rpm, *lbl_speed, *lbl_state;
static lv_obj_t *tile_val[4], *tile_cap[4], *cap_speed;
static int s_units_p0 = -1, s_units_p2 = -1;      /* units the captions show; -1 forces a refresh after a (re)build */
static lv_obj_t *lbl_foot, *bar_cpu, *lbl_pow, *bar_pow, *lbl_cpu_val, *lbl_pow_val;
/* ---- widgets, page 1 ---- */
static lv_obj_t *scr1, *lbl_info;
/* ---- widgets, page 2 (internals) ---- */
#define N_PAGES 5
#define P2_STD 12
#define P2_CUSTOM 6   /* 2 x 3 rows: y 212..291, footer 292..307, dots 308..314 */
#define P2_EVENTS 5
static lv_obj_t *scr2, *p2_val[P2_STD], *p2_cap[P2_STD], *p2_cval[P2_CUSTOM], *p2_ccap[P2_CUSTOM], *p2_foot;
static lv_obj_t *p2_ev_cap, *p2_ev_line[P2_EVENTS];
/* ---- widgets, page 3 (faults) ---- */
#define F_CARDS 2
static lv_obj_t *scr3, *f_head, *f_insight, *f_pos;
static lv_obj_t *f_card[F_CARDS], *f_code[F_CARDS], *f_sev[F_CARDS], *f_desc[F_CARDS], *f_cause[F_CARDS];
static int s_fault_idx;
static int64_t s_fault_next_ms;
static uint32_t s_fault_rev_shown;
/* ---- widgets, page 4 (system) ---- */
static lv_obj_t *scr4, *s4_head, *s4_stats, *s4_events;
/* ---- shared overlays ---- */
static lv_obj_t *banner, *banner_lbl, *prompt, *prompt_lbl, *hold_arc, *hold_lbl, *popup, *popup_lbl;
static lv_obj_t *dots[N_PAGES][N_PAGES];   /* [screen][dot] */
typedef struct { const char *cap; obd_field_t field; const char *fmt; } p2_item_t;
static const p2_item_t P2_ITEMS[P2_STD] = {
    {"TIMING\xC2\xB0", F_timing,    "%.1f"},
    {"STFT %",           F_stft1,     "%.1f"},
    {"LTFT %",           F_ltft1,     "%.1f"},
    {"LOAD %",           F_load,      "%.0f"},
    {"BOOST kPa",        F_map,       "%.0f"},      /* shown as MAP - BARO when both known */
    {"CAT \xC2\xB0" "C", F_cat_b1s1, "%.0f"},
    {"RAIL kPa",         F_rail_gauge,"%.0f"},
    {"FUEL L/h",         F_fuel_rate, "%.1f"},
    {"RUN min",          F_run_time,  "%.0f"},      /* seconds / 60 */
    {"MIL km",           F_dist_mil,  "%.0f"},
    {"PEDAL %",          F_pedal_d,   "%.0f"},
    {"ODO km",           F_odometer,  "%.0f"},
};

/* Re-colour every object that carries a colour from the old theme. Dynamic labels are repainted by
 * the refresh timer anyway; this handles the static ones (captions, tiles, bars, dots, borders).
 * Only properties the object set itself (local styles) are touched: reading an inherited or default colour and
 * writing it back would create a new local style on every object and part, which empties LVGL's memory pool
 * (CONFIG_LV_MEM_SIZE_KILOBYTES) and halts in LV_ASSERT_MALLOC until the task watchdog resets the board. */
static void remap_prop(lv_obj_t *o, lv_style_prop_t prop, lv_part_t part, const theme_t *from, const theme_t *to)
{
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(o, prop, &v, part) != LV_STYLE_RES_FOUND) return;
    const uint32_t *f = (const uint32_t *)from, *t = (const uint32_t *)to;
    const uint32_t have = lv_color_to_u32(v.color);
    for (int i = 0; i < (int)(sizeof(theme_t) / sizeof(uint32_t)); i++) {
        if (have != lv_color_to_u32(lv_color_hex(f[i]))) continue;
        v.color = lv_color_hex(t[i]);
        lv_obj_set_local_style_prop(o, prop, v, part);   /* same style, same slot: no allocation */
        return;
    }
}

static void remap_color(lv_obj_t *o, lv_part_t part, const theme_t *from, const theme_t *to)
{
    remap_prop(o, LV_STYLE_BG_COLOR, part, from, to);
    remap_prop(o, LV_STYLE_BORDER_COLOR, part, from, to);
    remap_prop(o, LV_STYLE_TEXT_COLOR, part, from, to);
    remap_prop(o, LV_STYLE_ARC_COLOR, part, from, to);
}

static void retheme_tree(lv_obj_t *o, const theme_t *from, const theme_t *to)
{
    remap_color(o, LV_PART_MAIN, from, to);
    remap_color(o, LV_PART_INDICATOR, from, to);
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) retheme_tree(lv_obj_get_child(o, i), from, to);
}

static lv_obj_t *s_screens[N_PAGES];

static void apply_theme(const theme_t *to)
{
    if (to == T) return;
    const theme_t *from = T;
    T = to;
    int64_t t0 = esp_timer_get_time();
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    /* colours first with style refreshing paused, then one refresh per tree: each set would otherwise re-lay out
       the object and its children, hundreds of times over */
    lv_obj_enable_style_refresh(false);
    for (int i = 0; i < N_PAGES; i++) if (s_screens[i]) retheme_tree(s_screens[i], from, to);
    retheme_tree(lv_layer_top(), from, to);
    lv_obj_enable_style_refresh(true);
    for (int i = 0; i < N_PAGES; i++) if (s_screens[i]) lv_obj_refresh_style(s_screens[i], LV_PART_ANY, LV_STYLE_PROP_ANY);
    lv_obj_refresh_style(lv_layer_top(), LV_PART_ANY, LV_STYLE_PROP_ANY);
    lv_mem_monitor(&after);
    ESP_LOGI(TAG, "theme -> %s in %lld ms, LVGL pool %u%% -> %u%% used of %u B", to == &THEME_LIGHT ? "light" : "dark",
             (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)before.used_pct, (unsigned)after.used_pct, (unsigned)after.total_size);
}

/* 0 auto (light between day_start and day_end once the clock is synced), 1 dark, 2 light */
static void theme_tick(const app_status_t *st)
{
    const remote_config_t *rc = remote_config_get();
    const theme_t *want = &THEME_DARK;
    if (rc->theme_mode == 2) want = &THEME_LIGHT;
    else if (rc->theme_mode == 0 && (st->time_synced || st->clock_estimated)) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        bool day = rc->day_start_hour < rc->day_end_hour ? (tm.tm_hour >= rc->day_start_hour && tm.tm_hour < rc->day_end_hour)
                                                         : (tm.tm_hour >= rc->day_start_hour || tm.tm_hour < rc->day_end_hour);
        if (day) want = &THEME_LIGHT;
    }
    apply_theme(want);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *make_tile(lv_obj_t *parent, int x, int y, int w, int h, const char *caption, lv_obj_t **val, lv_obj_t **cap)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
    lv_obj_set_style_border_width(t, 1, 0);                    /* the outline keeps tiles visible on the light page */
    lv_obj_set_style_border_color(t, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_radius(t, 8, 0);
    lv_obj_set_style_pad_all(t, 4, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    *cap = make_label(t, &lv_font_montserrat_12, C_MUTED);
    lv_label_set_text(*cap, caption);
    lv_obj_align(*cap, LV_ALIGN_TOP_LEFT, 0, -2);
    *val = make_label(t, &lv_font_montserrat_24, C_TEXT);
    lv_label_set_text(*val, "--");
    lv_obj_align(*val, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return t;
}

static void build_page0(void)
{
    s_units_p0 = -1;
    scr0 = make_screen();
    /* status bar */
    bar_top = lv_obj_create(scr0);
    lv_obj_set_pos(bar_top, 0, 0);
    lv_obj_set_size(bar_top, 172, 22);
    lv_obj_set_style_bg_color(bar_top, lv_color_hex(C_TILE), 0);
    lv_obj_set_style_border_width(bar_top, 1, 0);
    lv_obj_set_style_border_side(bar_top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar_top, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_radius(bar_top, 0, 0);
    lv_obj_set_style_pad_all(bar_top, 2, 0);
    lv_obj_remove_flag(bar_top, LV_OBJ_FLAG_SCROLLABLE);
    lbl_ble  = make_label(bar_top, &lv_font_montserrat_14, C_MUTED);
    lbl_sd   = make_label(bar_top, &lv_font_montserrat_14, C_MUTED);
    lbl_wifi = make_label(bar_top, &lv_font_montserrat_14, C_MUTED);
    lbl_clock= make_label(bar_top, &lv_font_montserrat_12, C_MUTED);
    lv_label_set_text(lbl_ble, LV_SYMBOL_BLUETOOTH);
    lv_label_set_text(lbl_sd, LV_SYMBOL_SD_CARD);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(lbl_ble, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_align(lbl_sd, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_align(lbl_wifi, LV_ALIGN_LEFT_MID, 46, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_t *brand = make_label(bar_top, &lv_font_montserrat_12, C_ACCENT);
    lv_label_set_text(brand, CARMIND_SCREEN_NAME);
    lv_obj_align_to(brand, lbl_wifi, LV_ALIGN_OUT_RIGHT_MID, 8, 0);   /* about two spaces after the Wi-Fi icon */
    lv_obj_align_to(lbl_clock, brand, LV_ALIGN_OUT_RIGHT_MID, 8, 0);  /* the clock follows the name, well inside the panel edge */
    lbl_clock_bold = make_label(bar_top, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align_to(lbl_clock_bold, lbl_clock, LV_ALIGN_TOP_LEFT, 1, 0);
    /* the faux bold narrows the gaps between digits: open them up by 1 px on both copies (same in both themes) */
    lv_obj_set_style_text_letter_space(lbl_clock, 1, 0);
    lv_obj_set_style_text_letter_space(lbl_clock_bold, 1, 0);

    /* RPM block */
    lv_obj_t *cap = make_label(scr0, &lv_font_montserrat_12, C_MUTED);
    lv_label_set_text(cap, "RPM");
    lv_obj_set_pos(cap, 8, 25);
    lbl_rpm = make_label(scr0, &lv_font_montserrat_40, C_TEXT);
    lv_label_set_text(lbl_rpm, "----");
    lv_obj_set_pos(lbl_rpm, 6, 36);
    bar_rpm = lv_bar_create(scr0);
    lv_obj_set_pos(bar_rpm, 8, 82);
    lv_obj_set_size(bar_rpm, 156, 8);
    lv_bar_set_range(bar_rpm, 0, 7000);
    lv_obj_set_style_bg_color(bar_rpm, lv_color_hex(C_TILE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_rpm, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_rpm, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_rpm, 4, LV_PART_INDICATOR);

    /* speed block */
    cap_speed = make_label(scr0, &lv_font_montserrat_12, C_MUTED);
    lv_label_set_text(cap_speed, "SPEED  km/h");
    lv_obj_set_pos(cap_speed, 8, 96);
    lbl_speed = make_label(scr0, &lv_font_montserrat_40, C_ACCENT);
    lv_label_set_text(lbl_speed, "--");
    lv_obj_set_pos(lbl_speed, 6, 107);

    /* 2x2 tiles */
    static const char *caps[4] = {"CAR TEMP " "\xC2\xB0" "C", "BATTERY V", "KM/L", "FUEL %"};   /* CAR TEMP = coolant, as car dashboards show it */
    for (int i = 0; i < 4; i++) {
        int x = 8 + (i % 2) * 80, y = 156 + (i / 2) * 52;
        make_tile(scr0, x, y, 76, 48, caps[i], &tile_val[i], &tile_cap[i]);
    }

    /* connection state line (shown while not logging) */

    /* footer: two rows above the page dots (y 308), each a name, a short bar and a value in their own columns
       (x 8-48 | 52-112 | 116-164), so text and bar never overlap. CPU is both cores together, out of 100 %; Power is
       the estimated board draw (cpu_monitor.c) on a 0-1 W bar, amber because it is an estimate. */
    static const char *names[2] = {"CPU", "Power"};
    lv_obj_t **bars[2] = {&bar_cpu, &bar_pow}, **vals[2] = {&lbl_cpu_val, &lbl_pow_val}, **nms[2] = {&lbl_foot, &lbl_pow};
    for (int i = 0; i < 2; i++) {
        int y = 266 + i * 20;
        *nms[i] = make_label(scr0, &lv_font_montserrat_12, C_MUTED);
        lv_label_set_text(*nms[i], names[i]);
        lv_obj_set_pos(*nms[i], 8, y);
        *bars[i] = lv_bar_create(scr0);
        lv_obj_set_size(*bars[i], 60, 8);
        lv_obj_set_pos(*bars[i], 52, y + 4);
        lv_bar_set_range(*bars[i], 0, 100);
        lv_obj_set_style_bg_color(*bars[i], lv_color_hex(C_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_color(*bars[i], lv_color_hex(i ? C_POWER : C_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_radius(*bars[i], 4, LV_PART_MAIN);
        lv_obj_set_style_radius(*bars[i], 4, LV_PART_INDICATOR);
        *vals[i] = make_label(scr0, &lv_font_montserrat_12, C_TEXT);
        lv_obj_set_size(*vals[i], 48, 15);
        lv_obj_set_pos(*vals[i], 116, y);
        lv_obj_set_style_text_align(*vals[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void build_page1(void)
{
    scr1 = make_screen();
    lv_obj_t *title = make_label(scr1, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(title, "Details");
    lv_obj_set_pos(title, 8, 6);
    lbl_info = make_label(scr1, &lv_font_montserrat_12, C_TEXT);
    lv_obj_set_width(lbl_info, 160);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_info, 6, 26);
}

static void add_dots(lv_obj_t *scr, int page)
{
    for (int i = 0; i < N_PAGES; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_set_size(d, 6, 6);
        lv_obj_set_style_radius(d, 3, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(i == page ? C_TEXT : C_BORDER), 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(d, LV_ALIGN_BOTTOM_RIGHT, -8 - (N_PAGES - 1 - i) * 10, -6);
        dots[page][i] = d;
    }
}

static void build_banner(void)
{
    banner = lv_obj_create(lv_layer_top());
    lv_obj_set_size(banner, 172, 40);
    lv_obj_set_pos(banner, 0, 0);
    lv_obj_set_style_bg_color(banner, lv_color_hex(C_BAD), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(banner, 0, 0);
    lv_obj_set_style_radius(banner, 0, 0);
    lv_obj_set_style_pad_all(banner, 4, 0);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    banner_lbl = make_label(banner, &lv_font_montserrat_14, C_BG);   /* screen background on red: 5:1 in both themes */
    lv_obj_set_width(banner_lbl, 160);
    lv_label_set_long_mode(banner_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(banner_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(banner_lbl);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);

    prompt = lv_obj_create(lv_layer_top());
    lv_obj_set_size(prompt, 172, 52);
    lv_obj_align(prompt, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(prompt, lv_color_hex(C_WARN), 0);
    lv_obj_set_style_bg_opa(prompt, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(prompt, 0, 0);
    lv_obj_set_style_radius(prompt, 0, 0);
    lv_obj_set_style_pad_all(prompt, 4, 0);
    lv_obj_remove_flag(prompt, LV_OBJ_FLAG_SCROLLABLE);
    prompt_lbl = make_label(prompt, &lv_font_montserrat_14, C_BG);
    lv_obj_set_width(prompt_lbl, 160);
    lv_label_set_long_mode(prompt_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(prompt_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(prompt_lbl);
    lv_obj_add_flag(prompt, LV_OBJ_FLAG_HIDDEN);

    popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(popup, 172, 34);
    lv_obj_set_pos(popup, 0, 22);
    lv_obj_set_style_bg_color(popup, lv_color_hex(C_WARN), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    lv_obj_set_style_radius(popup, 0, 0);
    lv_obj_set_style_pad_all(popup, 3, 0);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    popup_lbl = make_label(popup, &lv_font_montserrat_12, C_BG);
    lv_obj_set_width(popup_lbl, 164);
    lv_label_set_long_mode(popup_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(popup_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(popup_lbl);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);

    hold_arc = lv_arc_create(lv_layer_top());
    lv_obj_set_size(hold_arc, 96, 96);
    lv_obj_center(hold_arc);
    lv_arc_set_rotation(hold_arc, 270);
    lv_arc_set_bg_angles(hold_arc, 0, 360);
    lv_arc_set_range(hold_arc, 0, 100);
    lv_obj_remove_style(hold_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(hold_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(hold_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(hold_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(hold_arc, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(hold_arc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(hold_arc, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hold_arc, LV_OPA_80, LV_PART_MAIN);
    hold_lbl = make_label(hold_arc, &lv_font_montserrat_12, C_TEXT);
    lv_obj_set_style_text_align(hold_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(hold_lbl);
    lv_obj_add_flag(hold_arc, LV_OBJ_FLAG_HIDDEN);

    /* power-off overlay: the whole screen, so turning off is never mistaken for a frozen page */
    off_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(off_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(off_panel, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(off_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(off_panel, 0, 0);
    lv_obj_set_style_radius(off_panel, 0, 0);
    lv_obj_remove_flag(off_panel, LV_OBJ_FLAG_SCROLLABLE);
    off_title = make_label(off_panel, &lv_font_montserrat_24, C_TEXT);
    lv_obj_align(off_title, LV_ALIGN_CENTER, 0, -28);
    off_text = make_label(off_panel, &lv_font_montserrat_16, C_WARN);
    lv_obj_set_style_text_align(off_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(off_text, LV_ALIGN_CENTER, 0, 24);
    lv_obj_add_flag(off_panel, LV_OBJ_FLAG_HIDDEN);
}

static void build_page3(void)
{
    scr3 = make_screen();
    f_head = make_label(scr3, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(f_head, "Faults");
    lv_obj_set_pos(f_head, 8, 4);
    f_insight = make_label(scr3, &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_width(f_insight, 160);
    lv_label_set_long_mode(f_insight, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(f_insight, 6, 22);

    /* two compact cards: the two most important faults of the current pair */
    for (int c = 0; c < F_CARDS; c++) {
        lv_obj_t *card = lv_obj_create(scr3);
        lv_obj_set_pos(card, 6, 68 + c * 118);
        lv_obj_set_size(card, 160, 112);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_TILE), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(C_MUTED), 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        f_card[c] = card;
        f_code[c] = make_label(card, &lv_font_montserrat_18, C_TEXT);
        lv_obj_align(f_code[c], LV_ALIGN_TOP_LEFT, 0, 0);
        f_sev[c] = make_label(card, &lv_font_montserrat_12, C_BAD);
        lv_obj_align(f_sev[c], LV_ALIGN_TOP_RIGHT, 0, 3);
        f_desc[c] = make_label(card, &lv_font_montserrat_14, C_TEXT);
        lv_obj_set_width(f_desc[c], 148);
        lv_label_set_long_mode(f_desc[c], LV_LABEL_LONG_WRAP);
        lv_obj_align(f_desc[c], LV_ALIGN_TOP_LEFT, 0, 24);
        f_cause[c] = make_label(card, &lv_font_montserrat_12, C_ACCENT);
        lv_obj_set_size(f_cause[c], 148, 30);                       /* two lines max, then dots */
        lv_label_set_long_mode(f_cause[c], LV_LABEL_LONG_DOT);
        lv_obj_align(f_cause[c], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    f_pos = make_label(scr3, &lv_font_montserrat_12, C_MUTED);   /* which faults are showing, top right */
    lv_obj_set_size(f_pos, 80, 15);
    lv_obj_set_pos(f_pos, 84, 6);
    lv_obj_set_style_text_align(f_pos, LV_TEXT_ALIGN_RIGHT, 0);
    /* connection state and the MIL / DTC alert (moved here from page 1); long text scrolls, left of the page dots */
    lbl_state = make_label(scr3, &lv_font_montserrat_14, C_WARN);
    lv_obj_set_width(lbl_state, 104);
    lv_label_set_long_mode(lbl_state, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(lbl_state, 8, 300);
}

static void fill_card(int c, const fault_report_t *r, int idx)
{
    const fault_t *f = &r->faults[idx];
    uint32_t sev_col = f->severity >= SEV_CRITICAL ? C_BAD : f->severity == SEV_HIGH ? C_SEV_HIGH : f->severity == SEV_MEDIUM ? C_WARN : C_MUTED;
    lv_obj_remove_flag(f_card[c], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(f_card[c], lv_color_hex(sev_col), 0);
    lv_label_set_text_fmt(f_code[c], "%s%s", f->code, f->is_new ? " *" : "");
    lv_label_set_text(f_sev[c], fault_severity_name(f->severity));
    lv_obj_set_style_text_color(f_sev[c], lv_color_hex(sev_col), 0);
    lv_label_set_text(f_desc[c], f->desc);
    if (f->group >= 0) lv_label_set_text_fmt(f_cause[c], "%s: %s", r->groups[f->group].title, r->groups[f->group].cause);
    else lv_label_set_text(f_cause[c], f->cause);
}

static void refresh_page3(const app_status_t *st)
{
    const fault_report_t *r = &st->faults;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (st->fault_rev != s_fault_rev_shown) { s_fault_rev_shown = st->fault_rev; s_fault_idx = 0; s_fault_next_ms = now_ms + 6000; }
    lv_label_set_text_fmt(f_head, "Faults  %s", r->mil_on ? LV_SYMBOL_WARNING " MIL" : "");
    lv_obj_set_style_text_color(f_head, lv_color_hex(r->mil_on ? C_BAD : C_ACCENT), 0);
    lv_label_set_text(f_insight, r->insight[0] ? r->insight : "Waiting for the first DTC read...");

    if (r->n == 0) {
        for (int c = 0; c < F_CARDS; c++) lv_obj_add_flag(f_card[c], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(f_pos, "%lu rows", (unsigned long)st->rows_written);
        return;
    }
    /* pairs: the first pair is always the two most important; further pairs rotate every 6 s */
    int pairs = (r->n + F_CARDS - 1) / F_CARDS;
    if (now_ms >= s_fault_next_ms) { s_fault_idx = (s_fault_idx + 1) % pairs; s_fault_next_ms = now_ms + 6000; }
    if (s_fault_idx >= pairs) s_fault_idx = 0;
    int first = s_fault_idx * F_CARDS;
    for (int c = 0; c < F_CARDS; c++) {
        if (first + c < r->n) fill_card(c, r, first + c);
        else lv_obj_add_flag(f_card[c], LV_OBJ_FLAG_HIDDEN);
    }
    int last = first + F_CARDS < r->n ? first + F_CARDS : r->n;
    lv_label_set_text_fmt(f_pos, "%d-%d / %d", first + 1, last, r->n);   /* "*" after a code marks a new fault */
}

/* page 4: four fixed tiles, three event lines coloured by level, then two live strips: the soft real-time scheduler
 * (one row per registered job) and the on-board model. Tiles: CPU violet, PSRAM aqua / SYS TEMP green, UPTIME pink.
 * Strips: jobs sky, the classifier orange, the estimator red. Violet and sky, the closest pair for colour-blind
 * readers, never touch. */
#define S4_JOBS  3
static lv_obj_t *s4_tile_val[4], *s4_est_val[3], *s4_est_info;
static lv_obj_t *s4_job_dot[S4_JOBS], *s4_job_name[S4_JOBS], *s4_job_bar[S4_JOBS], *s4_job_val[S4_JOBS];
static lv_obj_t *s4_load, *s4_ml_class, *s4_ml_bar, *s4_ml_prob, *s4_ml_counts, *s4_ml_hist[ML_N_CLASSES];
static uint32_t s4_job_runs[S4_JOBS];
static uint8_t s4_job_age[S4_JOBS];        /* refreshes since the job's run count last moved */

static lv_obj_t *s4_frame(int y, int h, uint32_t col, const char *caption)
{
    lv_obj_t *t = lv_obj_create(scr4);
    lv_obj_set_pos(t, 4, y);
    lv_obj_set_size(t, 164, h);
    lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(col), 0);
    lv_obj_set_style_radius(t, 6, 0);
    lv_obj_set_style_pad_all(t, 3, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cap = make_label(t, &lv_font_montserrat_12, col);
    lv_label_set_text(cap, caption);
    lv_obj_set_pos(cap, 0, -2);
    return t;
}

static lv_obj_t *s4_label(lv_obj_t *parent, const lv_font_t *font, uint32_t col, int x, int y, int w, int h)
{
    lv_obj_t *l = make_label(parent, font, col);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *s4_bar(lv_obj_t *parent, int x, int y, int w, int h, int max)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, max);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
    return b;
}

static void build_page4(void)
{
    scr4 = make_screen();
    s4_head = make_label(scr4, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(s4_head, "System");
    lv_obj_set_pos(s4_head, 8, 4);
    static const char *caps[4] = {"MIPS", "PSRAM", "SYS TEMP", "UPTIME / FW"};    /* SYS TEMP = the chip's own sensor, page 1 CAR TEMP = coolant */
    const uint32_t cols[4] = {C_VIOLET, C_TEAL, C_SAGE, C_PINK};            /* TL TR / BL BR */
    for (int i = 0; i < 4; i++) {
        int x = 4 + (i % 2) * 83, y = 24 + (i / 2) * 46;
        lv_obj_t *t = lv_obj_create(scr4);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_size(t, 81, 42);
        lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(cols[i]), 0);
        lv_obj_set_style_radius(t, 6, 0);
        lv_obj_set_style_pad_all(t, 3, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *cap = make_label(t, &lv_font_montserrat_12, cols[i]);
        lv_label_set_text(cap, caps[i]);
        lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, -2);
        if (i == 0) {
            /* which core each number belongs to: SYS = core 0 (Bluetooth, Wi-Fi clock, display, scheduler),
               AI = core 1 (car data, the model, the learner). "MIPS" 31 px + "SYS/AI" 39 px fit the 73 px inside */
            lv_obj_t *who = make_label(t, &lv_font_montserrat_12, C_MUTED);
            lv_label_set_text(who, "SYS/AI");
            lv_obj_align(who, LV_ALIGN_TOP_RIGHT, 0, -2);
        }
        s4_tile_val[i] = make_label(t, &lv_font_montserrat_12, C_TEXT);
        lv_obj_set_size(s4_tile_val[i], 75, 16);                    /* fixed size: LONG_DOT only clips with both set */
        lv_label_set_long_mode(s4_tile_val[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s4_tile_val[i], LV_ALIGN_BOTTOM_LEFT, 0, 1);
    }
    /* what the model estimates, y 241..305 (content area 156 x 58): three rows, label left and value right.
       The recent events moved to the internals page. */
    lv_obj_t *est = s4_frame(241, 64, C_RED, "AI ESTIMATES");
    s4_est_info = s4_label(est, &lv_font_montserrat_12, C_MUTED, 96, -2, 60, 15);    /* right of the caption, never over it */
    lv_obj_set_style_text_align(s4_est_info, LV_TEXT_ALIGN_RIGHT, 0);
    static const char *est_names[3] = {"Brake pressure", "Engine torque", "Fuel rate"};
    for (int i = 0; i < 3; i++) {
        int y = 13 + i * 15;
        lv_obj_t *n = s4_label(est, &lv_font_montserrat_12, C_MUTED, 0, y, 92, 15);
        lv_label_set_text(n, est_names[i]);
        s4_est_val[i] = s4_label(est, &lv_font_montserrat_12, C_TEXT, 90, y, 66, 15);
        lv_obj_set_style_text_align(s4_est_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* scheduler strip, y 118..182 (content area 156 x 56): dot | name | time used vs. time allowed | last time or problem count */
    lv_obj_t *sch = s4_frame(118, 64, C_BLUE, "JOBS");
    s4_load = s4_label(sch, &lv_font_montserrat_12, C_MUTED, 36, -2, 120, 15);   /* right of "JOBS" (33 px) */
    lv_obj_set_style_text_align(s4_load, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < S4_JOBS; i++) {
        int y = 12 + i * 14;
        lv_obj_t *d = lv_obj_create(sch);
        lv_obj_set_pos(d, 0, y + 5);
        lv_obj_set_size(d, 6, 6);
        lv_obj_set_style_radius(d, 3, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(C_BORDER), 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        s4_job_dot[i] = d;
        s4_job_name[i] = s4_label(sch, &lv_font_montserrat_12, C_TEXT, 9, y, 54, 15);
        s4_job_bar[i] = s4_bar(sch, 66, y + 5, 32, 6, 1000);         /* per mille of the deadline */
        s4_job_val[i] = s4_label(sch, &lv_font_montserrat_12, C_MUTED, 102, y, 54, 15);
        lv_obj_set_style_text_align(s4_job_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* AI strip, y 185..238 (content area 156 x 45) */
    lv_obj_t *ai = s4_frame(185, 53, C_ORANGE, "AI SEES");
    s4_ml_class = s4_label(ai, &lv_font_montserrat_14, C_TEXT, 54, -3, 102, 17);
    lv_label_set_long_mode(s4_ml_class, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s4_ml_class, LV_TEXT_ALIGN_RIGHT, 0);   /* top right, above the class bars */
    s4_ml_bar = s4_bar(ai, 0, 19, 58, 6, 100);
    s4_ml_prob = s4_label(ai, &lv_font_montserrat_12, C_TEXT, 62, 14, 56, 15);
    s4_ml_counts = s4_label(ai, &lv_font_montserrat_12, C_MUTED, 0, 29, 116, 15);
    for (int k = 0; k < ML_N_CLASSES; k++) {
        int x = 120 + k * 9;
        s4_ml_hist[k] = s4_bar(ai, x, 15, 7, 16, 100);                /* taller than wide: LVGL draws it vertical */
        lv_obj_t *l = s4_label(ai, &lv_font_montserrat_12, C_MUTED, x - 1, 30, 9, 15);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text_fmt(l, "%c", toupper((unsigned char)ML_CLASSES[k][0]));
    }
    s4_stats = NULL;
    s4_events = NULL;
}

static void fmt_ms(char *buf, size_t n, uint32_t us)
{
    if (us < 10000) snprintf(buf, n, "%.1f", us / 1000.0f);
    else snprintf(buf, n, "%lu", (unsigned long)(us / 1000));
}

/* on-screen names for the jobs registered in sched_mon */
static const char *job_label(const char *name)
{
    if (strcmp(name, "obd_poll") == 0) return "Car data";
    if (strcmp(name, "ml") == 0) return "AI model";
    if (strcmp(name, "rev") == 0) return "Learning";
    return name;
}

static void refresh_sched(const sched_system_t *sys)
{
    /* per-core CPU load of the jobs, U = sum(C_max / T); amber above the rate-monotonic bound, red if the
       response-time analysis says some job can miss its deadline */
    lv_label_set_text_fmt(s4_load, "SYS/AI  %d%% / %d%%", (int)(sys->util[0] * 100.0f + 0.5f), (int)(sys->util[1] * 100.0f + 0.5f));   /* job load per core, same order as the MIPS tile; "SYS/AI  100% / 100%" is 118 of 120 px */
    bool over = sys->util[0] > sys->rm_bound[0] || sys->util[1] > sys->rm_bound[1];
    lv_obj_set_style_text_color(s4_load, lv_color_hex(!sys->schedulable ? C_BAD : over ? C_WARN : C_MUTED), 0);

    sched_stats_t js[S4_JOBS];
    int n = sched_get_stats(js, S4_JOBS);
    for (int i = 0; i < S4_JOBS; i++) {
        bool on = i < n;
        lv_obj_t *row[] = {s4_job_dot[i], s4_job_name[i], s4_job_bar[i], s4_job_val[i]};
        for (int k = 0; k < 4; k++) { if (on) lv_obj_remove_flag(row[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(row[k], LV_OBJ_FLAG_HIDDEN); }
        if (!on) continue;
        const sched_stats_t *j = &js[i];
        if (j->runs != s4_job_runs[i]) { s4_job_runs[i] = j->runs; s4_job_age[i] = 0; }
        else if (s4_job_age[i] < 255) s4_job_age[i]++;

        uint32_t ddl_us = j->deadline_ms * 1000u;
        bool late = j->runs && ddl_us && j->last_us > ddl_us;
        uint32_t fill = ddl_us ? (uint32_t)((uint64_t)j->last_us * 1000 / ddl_us) : 0;
        if (fill > 1000) fill = 1000;
        uint32_t bar_col = !j->expected ? C_MUTED : late ? C_BAD : fill > 700 ? C_WARN : C_OK;
        lv_bar_set_value(s4_job_bar[i], j->runs ? (int32_t)fill : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s4_job_bar[i], lv_color_hex(bar_col), LV_PART_INDICATOR);

        /* the dot flashes for ~300 ms each time the job completes a run */
        lv_obj_set_style_bg_color(s4_job_dot[i], lv_color_hex(!j->expected ? C_BORDER : late || j->stalls ? C_BAD : C_OK), 0);
        lv_obj_set_style_bg_opa(s4_job_dot[i], s4_job_age[i] < 3 ? LV_OPA_COVER : LV_OPA_30, 0);

        lv_label_set_text(s4_job_name[i], job_label(j->name));
        lv_obj_set_style_text_color(s4_job_name[i], lv_color_hex(j->expected ? C_TEXT : C_MUTED), 0);
        /* a problem count replaces the time: stuck = did not start for 3 periods, late = ran past its deadline */
        if (!j->expected) lv_label_set_text(s4_job_val[i], "idle");
        else if (j->stalls) lv_label_set_text_fmt(s4_job_val[i], "%lu stuck", (unsigned long)j->stalls);
        else if (j->misses) lv_label_set_text_fmt(s4_job_val[i], "%lu late", (unsigned long)j->misses);
        else if (!j->runs) lv_label_set_text(s4_job_val[i], "not yet");
        else {
            /* seconds, one decimal (23.5 s); runs under 0.1 s, such as an inference, show as "<0.1 s" */
            if (j->last_us < 100000) lv_label_set_text(s4_job_val[i], "<0.1 s");
            else {
                char sec[16];
                snprintf(sec, sizeof sec, "%.1f s", j->last_us / 1e6f);   /* libc printf: LVGL's may lack %f */
                lv_label_set_text(s4_job_val[i], sec);
            }
        }
        lv_obj_set_style_text_color(s4_job_val[i], lv_color_hex(!j->expected ? C_MUTED : (j->misses || j->stalls) ? C_WARN : C_TEXT), 0);
    }
}

static void refresh_ml(const sched_system_t *sys)
{
    ml_stats_t m;
    ml_get_stats(&m);
    float p[ML_N_CLASSES];
    int np = ml_get_last_probs(p, ML_N_CLASSES);
    bool have = m.ready && m.last_class >= 0 && m.last_class < ML_N_CLASSES && np == ML_N_CLASSES;

    if (!m.ready) {
        lv_label_set_text(s4_ml_class, sys->safe_mode ? "Safe mode" : "Model off");
        lv_obj_set_style_text_color(s4_ml_class, lv_color_hex(C_WARN), 0);
    } else if (!have) {
        lv_label_set_text(s4_ml_class, "No drive yet");
        lv_obj_set_style_text_color(s4_ml_class, lv_color_hex(C_MUTED), 0);
    } else {
        lv_label_set_text_fmt(s4_ml_class, "%c%s", toupper((unsigned char)ML_CLASSES[m.last_class][0]), ML_CLASSES[m.last_class] + 1);
        lv_obj_set_style_text_color(s4_ml_class, lv_color_hex(C_TEXT), 0);
    }

    int pct = have ? (int)(m.last_prob * 100.0f + 0.5f) : 0;
    lv_bar_set_value(s4_ml_bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s4_ml_bar, lv_color_hex(pct >= 70 ? C_OK : pct >= 45 ? C_WARN : C_BAD), LV_PART_INDICATOR);
    if (have) lv_label_set_text_fmt(s4_ml_prob, "%d%% sure", pct);
    else lv_label_set_text(s4_ml_prob, "");

    /* one check per second (sliding window); skipped = dropped because the previous check was still queued, or failed */
    uint32_t skipped = m.dropped + m.failed;
    if (skipped) lv_label_set_text_fmt(s4_ml_counts, "%lu checks, %lu skipped", (unsigned long)m.windows, (unsigned long)skipped);
    else if (m.windows) {
        char ms[12];
        fmt_ms(ms, sizeof ms, m.last_us);
        lv_label_set_text_fmt(s4_ml_counts, "%lu checks, %s ms", (unsigned long)m.windows, ms);
    } else lv_label_set_text(s4_ml_counts, "No checks yet");
    lv_obj_set_style_text_color(s4_ml_counts, lv_color_hex(skipped ? C_WARN : C_MUTED), 0);

    for (int k = 0; k < ML_N_CLASSES; k++) {
        lv_bar_set_value(s4_ml_hist[k], have ? (int32_t)(p[k] * 100.0f + 0.5f) : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s4_ml_hist[k], lv_color_hex(have && k == m.last_class ? C_ACCENT : C_MUTED), LV_PART_INDICATOR);
    }
}

static void refresh_est(const app_status_t *st)
{
    units_t un = (units_t)st->units;
    /* fuel rate follows the display units like the economy tile: L/h in metric, gal/h (US or imperial gallon) otherwise */
    double gal = un == UNITS_US ? 3.785411784 : un == UNITS_UK ? 4.54609 : 1.0;
    const char *unit[3] = {"bar", "Nm", un == UNITS_METRIC ? "L/h" : "gal/h"};
    static const char *fmt[3] = {"%.1f %s", "%.0f %s", "%.1f %s"};
    ml_stats_t m;
    ml_get_stats(&m);
    int64_t age_ms = esp_timer_get_time() / 1000 - m.last_at_ms;
    bool have = m.ready && m.windows > 0;
    bool fresh = have && age_ms < 3000;                 /* the model runs every second; older means paused or no data */
    for (int i = 0; i < 3; i++) {
        char buf[24];
        if (!have) snprintf(buf, sizeof buf, "--");
        else if (i == 0 && m.last_class == 0) snprintf(buf, sizeof buf, "holding");    /* stopped: hold pressure is not observable */
        else snprintf(buf, sizeof buf, fmt[i], (double)m.last_est[i] / (i == 2 ? gal : 1.0), unit[i]);
        uint32_t col = fresh ? C_TEXT : C_MUTED;
        /* R.E.V: the fuel row is the read-out it learns on the board, once there is one */
        if (i == 2 && have && m.model == ML_MODEL_REV && m.learned_ok && m.learned_lph >= 0)
            snprintf(buf, sizeof buf, fmt[2], (double)m.learned_lph / gal, unit[2]);
        /* R.E.V: torque from physics (rev_torque.h), not the network's head, which was trained on one engine. Amber: an
           estimate; "idle" at a standstill, where the physics reads high (torque-converter drag is not in it) */
        if (i == 1 && m.model == ML_MODEL_REV) {
            float tq = st->obd.torque_nm;
            if (!isfinite(tq)) snprintf(buf, sizeof buf, "--");
            else if (have && m.last_class == 0) snprintf(buf, sizeof buf, "idle");
            else snprintf(buf, sizeof buf, "%.0f Nm", (double)tq);
            if (isfinite(tq) && fresh && st->obd.torque_src != REV_TQ_PID) col = C_AMBER;
        }
        lv_label_set_text(s4_est_val[i], buf);
        lv_obj_set_style_text_color(s4_est_val[i], lv_color_hex(col), 0);
    }
    /* beside the caption only the time since the model's last answer: grey while it runs, amber when it has paused */
    char info[16];
    if (!m.ready) snprintf(info, sizeof info, "off");
    else if (!have) snprintf(info, sizeof info, "--");
    else if (age_ms < 10000) snprintf(info, sizeof info, "%.1f s", (double)age_ms / 1000.0);
    else if (age_ms < 600000) snprintf(info, sizeof info, "%lu s", (unsigned long)(age_ms / 1000));
    else snprintf(info, sizeof info, "%lu min", (unsigned long)(age_ms / 60000));
    lv_label_set_text(s4_est_info, info);
    lv_obj_set_style_text_color(s4_est_info, lv_color_hex(fresh ? C_MUTED : C_WARN), 0);
}

static void refresh_page4(const app_status_t *st)
{
    lv_label_set_text_fmt(s4_tile_val[0], "%.1f / %.1f", st->pmu_mips[0], st->pmu_mips[1]);   /* hardware counters: SYS (core 0) / AI (core 1) */
    uint32_t used_kb = st->psram_total_kb > st->psram_free_kb ? st->psram_total_kb - st->psram_free_kb : 0;
    lv_label_set_text_fmt(s4_tile_val[1], "%.1f / %.1f MB", used_kb / 1024.0, st->psram_total_kb / 1024.0);   /* "7.8 / 7.8 MB" is 70 of 75 px */
    if (isfinite(st->chip_temp_c)) {
        bool us = (units_t)st->units == UNITS_US;
        lv_label_set_text_fmt(s4_tile_val[2], "%.0f \xC2\xB0%c", us ? st->chip_temp_c * 9.0f / 5.0f + 32.0f : st->chip_temp_c, us ? 'F' : 'C');
    } else lv_label_set_text(s4_tile_val[2], "--");
    lv_label_set_text_fmt(s4_tile_val[3], "%lum  v" CARMIND_VERSION, (unsigned long)(esp_timer_get_time() / 60000000));
    sched_system_t sys;
    sched_get_system(&sys);
    refresh_sched(&sys);
    refresh_ml(&sys);
    refresh_est(st);
}

static void build_page2(void)
{
    s_units_p2 = -1;
    scr2 = make_screen();
    lv_obj_t *title = make_label(scr2, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(title, "Internals");
    lv_obj_set_pos(title, 8, 4);
    for (int i = 0; i < P2_STD; i++) {
        int x = 4 + (i % 3) * 55, y = 22 + (i / 3) * 47;
        lv_obj_t *t = lv_obj_create(scr2);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_size(t, 53, 44);
        lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_radius(t, 6, 0);
        lv_obj_set_style_pad_all(t, 3, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        p2_cap[i] = make_label(t, &lv_font_montserrat_12, C_MUTED);
        lv_label_set_text(p2_cap[i], P2_ITEMS[i].cap);
        lv_obj_align(p2_cap[i], LV_ALIGN_TOP_LEFT, 0, -1);
        p2_val[i] = make_label(t, &lv_font_montserrat_18, C_TEXT);
        lv_label_set_text(p2_val[i], "--");
        lv_obj_align(p2_val[i], LV_ALIGN_BOTTOM_LEFT, 0, 1);
    }
    for (int i = 0; i < P2_CUSTOM; i++) {
        int x = 4 + (i % 2) * 83, y = 212 + (i / 2) * 27;
        lv_obj_t *t = lv_obj_create(scr2);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_size(t, 81, 25);
        lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_radius(t, 6, 0);
        lv_obj_set_style_pad_all(t, 2, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        p2_ccap[i] = make_label(t, &lv_font_montserrat_12, C_MUTED);
        lv_obj_align(p2_ccap[i], LV_ALIGN_LEFT_MID, 0, 0);
        p2_cval[i] = make_label(t, &lv_font_montserrat_16, C_TEXT);
        lv_obj_align(p2_cval[i], LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
        /* keep a handle to the tile through the caption's parent */
    }
    p2_ev_cap = make_label(scr2, &lv_font_montserrat_12, C_MUTED);
    lv_label_set_text(p2_ev_cap, "RECENT EVENTS");
    for (int i = 0; i < P2_EVENTS; i++) {
        p2_ev_line[i] = make_label(scr2, &lv_font_montserrat_12, C_TEXT);
        lv_obj_set_size(p2_ev_line[i], 160, 15);                     /* one fixed line each: never wraps, clips with dots */
        lv_label_set_long_mode(p2_ev_line[i], LV_LABEL_LONG_DOT);
    }
    p2_foot = make_label(scr2, &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(p2_foot, 8, 292);                                /* one line above the page dots (y 308) */
}

static void refresh_page2(const app_status_t *st)
{
    const obd_values_t *o = &st->obd;
    bool live = st->ble == BLE_ST_LOGGING;
    units_t un = (units_t)st->units;
    bool new_units = s_units_p2 != (int)un;
    s_units_p2 = (int)un;
    for (int i = 0; i < P2_STD; i++) {
        const p2_item_t *it = &P2_ITEMS[i];
        if (new_units && it->field == F_odometer) lv_label_set_text(p2_cap[i], un == UNITS_METRIC ? "ODO km" : "ODO mi");
        if (new_units && it->field == F_dist_mil) lv_label_set_text(p2_cap[i], un == UNITS_METRIC ? "MIL km" : "MIL mi");
        if (it->field == F_fuel_rate) lv_obj_set_style_text_color(p2_val[i], lv_color_hex(C_TEXT), 0);   /* amber only below */
        if (it->field == F_fuel_rate && live && !OBD_HAS(o, it->field) && o->fuel_lph_calc >= 0) {
            /* no fuel-rate PID: show the rate worked out from MAF (or the model, in amber) */
            lv_label_set_text_fmt(p2_val[i], "%.1f", (double)o->fuel_lph_calc);
            lv_obj_set_style_text_color(p2_val[i], lv_color_hex(econ_src_is_estimate((fuel_src_t)o->fuel_src) ? C_WARN : C_TEXT), 0);
            continue;
        }
        if (!live || !OBD_HAS(o, it->field)) { lv_label_set_text(p2_val[i], "--"); continue; }
        float v = o->f[it->field];
        if (it->field == F_odometer || it->field == F_dist_mil) v = (float)econ_dist_to_unit(v, un);
        if (it->field == F_map) {
            if (!OBD_HAS(o, F_baro)) { lv_label_set_text(p2_val[i], "--"); continue; }
            v = o->f[F_map] - o->f[F_baro];
        } else if (it->field == F_run_time) v /= 60.0f;
        lv_label_set_text_fmt(p2_val[i], it->fmt, v);
    }
    if (st->pattern_windows) lv_label_set_text_fmt(p2_foot, "pattern %d  novelty %.1f  (%u/%lu)", st->pattern_id, st->pattern_novelty, st->pattern_used, (unsigned long)st->pattern_windows);
    else lv_label_set_text(p2_foot, "pattern: first window pending");
    for (int i = 0; i < P2_CUSTOM; i++) {
        lv_obj_t *tile = lv_obj_get_parent(p2_ccap[i]);
        if (i >= o->custom_count) { lv_obj_add_flag(tile, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(p2_ccap[i], "%.9s", o->custom_name[i]);
        if (live && (o->custom_valid & (1 << i))) lv_label_set_text_fmt(p2_cval[i], "%.4g%s", o->custom[i], o->custom_unit[i][0] ? o->custom_unit[i] : "");
        else lv_label_set_text(p2_cval[i], "--");
    }

    /* recent events fill the space under the custom tiles: caption plus as many lines as fit above the footer (y 292) */
    int shown = o->custom_count < P2_CUSTOM ? o->custom_count : P2_CUSTOM;
    int top = 212 + ((shown + 1) / 2) * 27;
    int fit = (292 - (top + 16)) / 15;
    if (fit > P2_EVENTS) fit = P2_EVENTS;
    if (fit < 1) lv_obj_add_flag(p2_ev_cap, LV_OBJ_FLAG_HIDDEN);
    else { lv_obj_remove_flag(p2_ev_cap, LV_OBJ_FLAG_HIDDEN); lv_obj_set_pos(p2_ev_cap, 6, top); }
    bool save_line_shown = false;
    for (int i = 0; i < P2_EVENTS; i++) {
        if (i >= fit) { lv_obj_add_flag(p2_ev_line[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(p2_ev_line[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(p2_ev_line[i], 6, top + 16 + i * 15);
        if (i >= st->event_count) { lv_label_set_text(p2_ev_line[i], i == 0 ? "no events yet" : ""); lv_obj_set_style_text_color(p2_ev_line[i], lv_color_hex(C_MUTED), 0); continue; }
        const sys_event_t *e = &st->events[(st->event_head + EVENT_MAX - 1 - i) % EVENT_MAX];   /* newest first */
        unsigned long mm = (unsigned long)(e->uptime_ms / 60000), ss = (unsigned long)((e->uptime_ms / 1000) % 60);
        if (strcmp(e->text, "Safe to turn off") == 0) {
            /* The newest save line shows the state now, not when it was written: green only when car data has stopped,
               everything is on the card and nothing is uploading (power_state.c). Older copies are greyed. */
            if (!save_line_shown) {
                save_line_shown = true;
                power_state_t ps = power_state(sdlog_ok(), sdlog_saved(), sdlog_ms_since_write(), sys_mode_get() == MODE_DRIVE,
                                               uploader_busy());
                lv_label_set_text_fmt(p2_ev_line[i], "%lu:%02lu %s %s", mm, ss, ps == POWER_SAFE ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                                      power_state_text(ps));
                lv_obj_set_style_text_color(p2_ev_line[i], lv_color_hex(ps == POWER_SAFE ? C_OK : C_BAD), 0);
            } else {
                lv_label_set_text_fmt(p2_ev_line[i], "%lu:%02lu Saved", mm, ss);
                lv_obj_set_style_text_color(p2_ev_line[i], lv_color_hex(C_MUTED), 0);
            }
            continue;
        }
        lv_label_set_text_fmt(p2_ev_line[i], "%lu:%02lu %s", mm, ss, e->text);
        lv_obj_set_style_text_color(p2_ev_line[i], lv_color_hex(e->level == EV_ERROR ? C_BAD : e->level == EV_WARN ? C_WARN : C_TEXT), 0);
    }
}

static ble_state_t s_last_ble_state = -1;
static int64_t s_state_since_ms;

static const char *ble_state_text(ble_state_t s, const app_status_t *st)
{
    static char buf[72];
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s != s_last_ble_state) { s_last_ble_state = s; s_state_since_ms = now_ms; }
    int secs = (int)((now_ms - s_state_since_ms) / 1000);
    switch (s) {
    case BLE_ST_SCANNING:
        if (st->adapter_saved) snprintf(buf, sizeof buf, "Searching %s  %d s", st->adapter_name[0] ? st->adapter_name : "saved adapter", secs);
        else snprintf(buf, sizeof buf, "Searching for adapter  %d s", secs);
        return buf;
    case BLE_ST_CONNECTING:  snprintf(buf, sizeof buf, "Connecting  %d s", secs); return buf;
    case BLE_ST_DISCOVERING: snprintf(buf, sizeof buf, "Reading services  %d s", secs); return buf;
    case BLE_ST_ELM_INIT:    snprintf(buf, sizeof buf, "Waiting for ECU, ignition on?  %d s", secs); return buf;
    case BLE_ST_LOGGING:     snprintf(buf, sizeof buf, "Logging  %s", st->obd.protocol[0] ? st->obd.protocol : ""); return buf;
    case BLE_ST_LOCKED:      return "LELink locked: disable password in LELink Config app";
    default:                 return "Starting...";
    }
}

static lv_timer_t *s_refresh_timer;

static lv_obj_t *page_screen(int page)
{
    return page == 0 ? scr0 : page == 1 ? scr1 : page == 2 ? scr2 : page == 3 ? scr3 : scr4;
}

static void set_clock(const char *text, uint32_t color)
{
    lv_label_set_text(lbl_clock, text);
    lv_label_set_text(lbl_clock_bold, text);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(lbl_clock_bold, lv_color_hex(color), 0);
}

static void refresh_cb(lv_timer_t *t)
{
    EXT_RAM_BSS_ATTR static app_status_t st;   /* ~3 KB snapshot in PSRAM, off the LVGL task stack */
    status_get(&st);
    theme_tick(&st);

    if (s_shown_page != s_page) {
        s_shown_page = s_page;
        lv_screen_load(page_screen(s_page));   /* instant: a fade draws both pages over each other for 120 ms */
    }

    /* status icons */
    lv_obj_set_style_text_color(lbl_ble, lv_color_hex(st.ble == BLE_ST_LOGGING ? C_OK : st.ble == BLE_ST_LOCKED ? C_BAD : st.ble >= BLE_ST_CONNECTING ? C_WARN : st.ble == BLE_ST_IDLE ? C_DIM : C_MUTED), 0);
    lv_obj_set_style_text_color(lbl_sd, lv_color_hex(st.sd_mounted ? C_OK : C_BAD), 0);
    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(st.wifi == WIFI_ST_CONNECTED ? C_OK :
                                                       st.wifi == WIFI_ST_CONNECTING ? C_WARN : st.wifi == WIFI_ST_DISABLED ? C_DIM : C_MUTED), 0);

    if (st.time_synced || st.clock_estimated) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char hhmm[8];
        snprintf(hhmm, sizeof hhmm, "%02d:%02d", tm.tm_hour, tm.tm_min);
        set_clock(hhmm, st.time_synced ? C_TEXT : C_WARN);                                         /* amber: estimated */
    } else {
        set_clock("--:--", C_MUTED);                      /* no clock until the first Wi-Fi time sync */
    }

    const obd_values_t *o = &st.obd;
    bool live = st.ble == BLE_ST_LOGGING;
    if (live && OBD_HAS(o, F_rpm)) {
        int rpm = (int)o->f[F_rpm];
        lv_label_set_text_fmt(lbl_rpm, "%d", rpm);
        lv_bar_set_value(bar_rpm, rpm, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar_rpm, lv_color_hex(rpm > 5500 ? C_BAD : rpm > 4000 ? C_WARN : C_ACCENT), LV_PART_INDICATOR);
    } else { lv_label_set_text(lbl_rpm, "----"); lv_bar_set_value(bar_rpm, 0, LV_ANIM_OFF); }

    units_t un = (units_t)st.units;
    bool new_units = s_units_p0 != (int)un;
    s_units_p0 = (int)un;
    if (new_units) lv_label_set_text(cap_speed, un == UNITS_METRIC ? "SPEED  km/h" : "SPEED  mph");
    if (live && OBD_HAS(o, F_speed)) lv_label_set_text_fmt(lbl_speed, "%d", (int)(econ_dist_to_unit(o->f[F_speed], un) + 0.5));
    else lv_label_set_text(lbl_speed, "--");

    if (live && OBD_HAS(o, F_coolant)) {
        float c = o->f[F_coolant];
        lv_label_set_text_fmt(tile_val[0], "%d", (int)lroundf(un == UNITS_US ? c * 9.0f / 5.0f + 32.0f : c));   /* limits stay in C */
        lv_obj_set_style_text_color(tile_val[0], lv_color_hex(c > 105 ? C_BAD : c < 60 ? C_ACCENT : C_TEXT), 0);
    } else lv_label_set_text(tile_val[0], "--");

    if (live && o->adapter_v_valid) {
        lv_label_set_text_fmt(tile_val[1], "%.1f", o->adapter_voltage_v);
        lv_obj_set_style_text_color(tile_val[1], lv_color_hex(o->adapter_voltage_v < 12.0f ? C_WARN : C_TEXT), 0);
    } else if (live && OBD_HAS(o, F_modv)) lv_label_set_text_fmt(tile_val[1], "%.1f", o->f[F_modv]);
    else lv_label_set_text(tile_val[1], "--");

    /* economy: the drive's average, or the instant value until enough is counted; amber when it rests on the model */
    if (new_units) lv_label_set_text(tile_cap[2], un == UNITS_METRIC ? "KM/L" : un == UNITS_UK ? "MPG (UK)" : "MPG");
    if (new_units) lv_label_set_text(tile_cap[0], un == UNITS_US ? "CAR TEMP \xC2\xB0" "F" : "CAR TEMP \xC2\xB0" "C");
    {
        float kpl = o->econ_trip_kpl >= 0 ? o->econ_trip_kpl : o->econ_inst_kpl;
        if (live && kpl >= 0) {
            lv_label_set_text_fmt(tile_val[2], "%.1f", (double)econ_to_unit(kpl, un));
            lv_obj_set_style_text_color(tile_val[2], lv_color_hex(econ_src_is_estimate((fuel_src_t)o->fuel_src) ? C_WARN : C_TEXT), 0);
        } else { lv_label_set_text(tile_val[2], "--"); lv_obj_set_style_text_color(tile_val[2], lv_color_hex(C_TEXT), 0); }
    }
    if (live && OBD_HAS(o, F_fuel_level)) lv_label_set_text_fmt(tile_val[3], "%d", (int)o->f[F_fuel_level]);
    else lv_label_set_text(tile_val[3], "--");

    lv_label_set_text(lbl_state, ble_state_text(st.ble, &st));
    if (live && o->status_valid && (o->mil_on || o->dtc_count)) {
        lv_label_set_text_fmt(lbl_state, LV_SYMBOL_WARNING " MIL %s  %u DTC %s", o->mil_on ? "ON" : "off", o->dtc_count, o->dtcs);
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(C_BAD), 0);
    } else lv_obj_set_style_text_color(lbl_state, lv_color_hex(live ? C_OK : C_WARN), 0);

    {
        int cpu = (st.cpu_pct[0] + st.cpu_pct[1] + 1) / 2;                 /* both cores together, out of 100 % */
        int pw = (int)lroundf(st.power_w * 100.0f);                          /* 0-1 W bar */
        lv_bar_set_value(bar_cpu, cpu, LV_ANIM_OFF);
        lv_bar_set_value(bar_pow, pw > 100 ? 100 : pw, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar_cpu, lv_color_hex(cpu > 85 ? C_BAD : cpu > 60 ? C_WARN : C_ACCENT), LV_PART_INDICATOR);
        lv_label_set_text_fmt(lbl_cpu_val, "%d %%", cpu);
        lv_label_set_text_fmt(lbl_pow_val, "%.2f W", st.power_w);
        /* no SD card: the CPU row becomes the warning, across the full width */
        bool sd = st.sd_mounted;
        if (!sd) lv_label_set_text_fmt(lbl_foot, "SD: %s", st.sd_error[0] ? st.sd_error : "not mounted");
        else lv_label_set_text(lbl_foot, "CPU");
        lv_obj_set_style_text_color(lbl_foot, lv_color_hex(sd ? C_MUTED : C_WARN), 0);
        if (sd) { lv_obj_remove_flag(bar_cpu, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(lbl_cpu_val, LV_OBJ_FLAG_HIDDEN); }
        else    { lv_obj_add_flag(bar_cpu, LV_OBJ_FLAG_HIDDEN);    lv_obj_add_flag(lbl_cpu_val, LV_OBJ_FLAG_HIDDEN); }
        static uint8_t applied_bl;
        if (remote_config_get()->backlight_pct != applied_bl) { applied_bl = remote_config_get()->backlight_pct; display_set_backlight(applied_bl); }
    }

    if (s_page == 1) {
        static const char *wifi_txt[] = {"disabled", "scanning", "connecting", "connected", "idle"};
        /* fuel used this drive and since the board started counting (rev_cal.h), with economy in the board's units:
           L and L/100 km in metric, gallons and mpg in the US and the UK */
        char fuel_drive[40], fuel_life[48], tank[40];
        rev_status_t rs;
        rev_get_status(&rs);
        units_t un = (units_t)st.units;
        double gal = un == UNITS_US ? 3.785411784 : un == UNITS_UK ? 4.54609 : 1.0;
        const char *vol = un == UNITS_METRIC ? "L" : "gal";
        double d_l = o->trip_fuel_l, d_km = o->trip_km;
        if (d_l >= 0.02 && d_km >= 0.5) {
            if (un == UNITS_METRIC) snprintf(fuel_drive, sizeof fuel_drive, "Drive %.2f L  %.1f L/100km", d_l, 100.0 * d_l / d_km);
            else snprintf(fuel_drive, sizeof fuel_drive, "Drive %.2f gal  %.1f mpg", d_l / gal, (double)econ_to_unit((float)(d_km / d_l), un));
        } else snprintf(fuel_drive, sizeof fuel_drive, "Drive %.2f %s", d_l / gal, vol);
        if (rs.life_l >= 0.02 && rs.life_km >= 0.5) {
            if (un == UNITS_METRIC) snprintf(fuel_life, sizeof fuel_life, "Total %.1f L  %.1f L/100km x%.2f", rs.life_l, 100.0 * rs.life_l / rs.life_km, (double)rs.cal);
            else snprintf(fuel_life, sizeof fuel_life, "Total %.1f gal  %.1f mpg x%.2f", rs.life_l / gal, (double)econ_to_unit((float)(rs.life_km / rs.life_l), un), (double)rs.cal);
        } else snprintf(fuel_life, sizeof fuel_life, "Total %.1f %s  x%.2f", rs.life_l / gal, vol, (double)rs.cal);
        /* tank learner (rev_tank.h): litres left and range once the tank size is known, else how far it has got */
        if (rs.tank.ready && rs.tank.left_raw_l >= 0) {
            double left = rs.tank.left_raw_l * rs.cal / gal, km = rs.tank.range_km;
            if (km >= 0) snprintf(tank, sizeof tank, "Tank %.1f %s  %.0f %s", left, vol, un == UNITS_METRIC ? km : km / 1.609344,
                                  un == UNITS_METRIC ? "km" : "mi");
            else snprintf(tank, sizeof tank, "Tank %.1f %s left", left, vol);
        } else snprintf(tank, sizeof tank, "Tank: learning %.1f of %.0f %s", rs.tank.band_l * rs.cal / gal, REV_TANK_READY_L / gal, vol);
        lv_label_set_text_fmt(lbl_info,
            "Adapter: %s\n" DISPLAY_ADAPTER_FMT "  rssi %d\n\nVIN: %s\nProto: %s\n\nMIL %s, %u DTC\nstored: %s\npending: %s\n"
            "Wi-Fi: %s %s\nClock: %s\nSD: %s\n\nSession s%06lu  %lu rows  up %lu min\nIntake %d\xC2\xB0  Oil %d\xC2\xB0  Amb %d\xC2\xB0\nThr %d%%  MAF %.1f g/s\n%s\n%s\n%s\n" CARMIND_SCREEN_NAME " " CARMIND_VERSION "\nBOOT: tap = page, 2 s = off, 8 s = forget",
            st.adapter_name[0] ? st.adapter_name : "-", DISPLAY_ADAPTER_ARGS(st), st.adapter_rssi,
            o->vin[0] ? o->vin : "-", o->protocol[0] ? o->protocol : "-",
            o->mil_on ? "ON" : "off", o->dtc_count, o->dtcs[0] ? o->dtcs : "none", o->pending_dtcs[0] ? o->pending_dtcs : "none",
            wifi_txt[st.wifi], (st.wifi == WIFI_ST_CONNECTING || st.wifi == WIFI_ST_CONNECTED) ? st.wifi_ssid : "", st.time_synced ? "synced" : "not synced",
            st.sd_mounted ? "ok" : (st.sd_error[0] ? st.sd_error : "not mounted"),
            (unsigned long)st.session_id, (unsigned long)st.rows_written, (unsigned long)(esp_timer_get_time() / 60000000),
            (int)o->f[F_iat], (int)o->f[F_oil], (int)o->f[F_ambient], (int)o->f[F_throttle], o->f[F_maf], fuel_drive, fuel_life, tank);
    } else if (s_page == 2) {
        refresh_page2(&st);
    } else if (s_page == 3) {
        refresh_page3(&st);
    } else if (s_page == 4) {
        refresh_page4(&st);
    }

    /* overlays shown on every page */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (st.alarm_text[0] && now_ms < st.alarm_until_ms) {
        lv_label_set_text_fmt(banner_lbl, "%s%s", st.alarm_text, st.alarm_critical ? "  (tap = OK)" : "");
        lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);

    if (st.sys_popup[0] && now_ms < st.sys_popup_until_ms && lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text(popup_lbl, st.sys_popup);
        lv_obj_set_style_bg_color(popup, lv_color_hex(st.sys_popup_error ? C_BAD : C_WARN), 0);
        lv_obj_set_style_text_color(popup_lbl, lv_color_hex(C_BG), 0);
        lv_obj_remove_flag(popup, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);

    if (st.prompt_text[0] && now_ms < st.prompt_until_ms) {
        lv_label_set_text(prompt_lbl, st.prompt_text);
        lv_obj_remove_flag(prompt, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(prompt, LV_OBJ_FLAG_HIDDEN);

    if (st.hold_pct > 0) {
        lv_arc_set_value(hold_arc, st.hold_pct);
        lv_obj_set_style_arc_color(hold_arc, lv_color_hex(st.hold_stage == 2 ? C_VIOLET : st.hold_stage == 1 ? C_BAD : C_ACCENT), LV_PART_INDICATOR);
        lv_label_set_text(hold_lbl, st.hold_stage == 2 ? "release:\nforget\nadapter" : st.hold_stage == 1 ? "release:\npower off" : "hold...");
        lv_obj_remove_flag(hold_arc, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(hold_arc, LV_OBJ_FLAG_HIDDEN);

    if (st.shutdown_stage) {
        lv_label_set_text(off_title, st.shutdown_stage == 1 ? "Turning off" : "Off");
        lv_label_set_text(off_text, st.shutdown_stage == 1 ? "Saving the drive.\nDon't unplug yet." : "Safe to unplug.\nPress BOOT to start.");
        lv_obj_set_style_text_color(off_text, lv_color_hex(st.shutdown_stage == 1 ? C_WARN : C_OK), 0);
        lv_obj_align(off_title, LV_ALIGN_CENTER, 0, -28);
        lv_obj_align(off_text, LV_ALIGN_CENTER, 0, 24);
        lv_obj_add_flag(hold_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(off_panel, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(off_panel, LV_OBJ_FLAG_HIDDEN);
}

static void backlight_init(void)
{
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);
    ledc_channel_config_t cc = {
        .gpio_num = BOARD_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = (1023 * remote_config_get()->backlight_pct) / 100, .hpoint = 0,
    };
    ledc_channel_config(&cc);
}

esp_err_t display_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCLK, .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * 24 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC, .cs_gpio_num = BOARD_LCD_CS, .pclk_hz = BOARD_LCD_PCLK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io), TAG, "io");

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = BOARD_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &pcfg, &panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    esp_lcd_panel_set_gap(panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y);
    #ifdef CONFIG_LCD_INVERT_COLORS
    esp_lcd_panel_invert_color(panel, true);
#else
    esp_lcd_panel_invert_color(panel, false);
#endif
    esp_lcd_panel_disp_on_off(panel, true);
    s_panel = panel;
    backlight_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 12288;
    port_cfg.task_priority = 4;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");
    const lvgl_port_display_cfg_t dcfg = {
        .io_handle = io, .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * 24, .double_buffer = true,
        .hres = BOARD_LCD_H_RES, .vres = BOARD_LCD_V_RES, .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&dcfg);
    if (!s_disp) return ESP_FAIL;

    lvgl_port_lock(0);
    build_page0();
    build_page1();
    build_page2();
    build_page3();
    build_page4();
    add_dots(scr0, 0); add_dots(scr1, 1); add_dots(scr2, 2); add_dots(scr3, 3); add_dots(scr4, 4);
    build_banner();
    s_screens[0] = scr0; s_screens[1] = scr1; s_screens[2] = scr2; s_screens[3] = scr3; s_screens[4] = scr4;
    lv_screen_load(scr0);
    s_shown_page = 0;
    s_refresh_timer = lv_timer_create(refresh_cb, 100, NULL);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

int display_current_page(void) { return s_page; }

void display_next_page(void)
{
    s_page = (s_page + 1) % N_PAGES;
    /* switch right away so the button feels immediate; the timer would otherwise add up to 100 ms */
    if (lvgl_port_lock(50)) {
        s_shown_page = s_page;
        lv_screen_load(page_screen(s_page));   /* instant: a fade draws both pages over each other for 120 ms */
        if (s_refresh_timer) lv_timer_ready(s_refresh_timer);   /* fill the new page now, not up to 100 ms later */
        lvgl_port_unlock();
    }
}

void display_set_backlight(uint8_t pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023 * pct) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_fade_off(int ms)
{
    int start = remote_config_get()->backlight_pct, steps = ms / 20 > 0 ? ms / 20 : 1;
    for (int i = steps - 1; i >= 0; i--) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023 * start * i) / (100 * steps));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
    gpio_set_direction(BOARD_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LCD_BL, 0);
    gpio_hold_en(BOARD_LCD_BL);                                   /* the backlight stays dark while the chip sleeps */
}

void display_sleep(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
}
