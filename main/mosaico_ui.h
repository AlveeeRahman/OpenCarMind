/* ESP-Mosaico launcher UI (ESP32-S31 only): a home screen (status bar, widget card, app grid) and full-screen apps.
 *
 * An app is one registered struct (mosaico_apps.c): create() builds its widgets into a full-screen parent, update()
 * refreshes them from a snapshot, destroy() (optional) forgets the widget pointers. Only the visible app is updated, at
 * its own period. Apps never read producers directly: the shell copies one ui_snap_t from the firmware's published
 * snapshots (status_get: the status mutex held for a memcpy; ml_get_stats, rev_get_status,
 * mosaico_gauge_read: a lock or spinlock held for a copy) and hands it to update(). Everything runs in the LVGL task
 * (esp_lvgl_port), so no LVGL call happens elsewhere; other tasks only set flags (display_next_page).
 *
 * Lock order (one global order, never reversed): LVGL port lock -> status mutex / ML / R.E.V locks. No producer
 * takes the LVGL lock, so the UI can wait on a producer for at most one memcpy, and a producer never waits on the UI. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "status.h"
#include "ml_infer.h"
#include "rev_task.h"
#include "mosaico_board.h"

/* ---- theme: every colour, font, radius and border of the launcher in one table per palette (mosaico_display.c:
   THEME_DARK, THEME_LIGHT). Restyling = editing those tables; apps use only these names. Three looks share a table:
   style A "Night drive" (every app), style B "Pixel garage" (Drive, b_*), style C "Cockpit" (home, c_*). ---- */
typedef enum { SEC_SKY = 0, SEC_ORANGE, SEC_GREEN, SEC_PINK, SEC_AMBER, SEC_RED, SEC_AQUA, SEC_VIOLET, SEC_CYAN, SEC_COUNT } ui_section_t;
typedef struct {
    const char *name;
    /* style A */
    uint32_t bg, card, border, text, muted, dim, track, good, warn, bad, estimate, ai, scrim, banner;
    uint32_t section[SEC_COUNT];                 /* per-app accents */
    uint32_t cls[4];                             /* stopped, braking, accelerating, cruising */
    uint32_t icon, icon_glyph;
    /* app surfaces without a style A name (Drive's panels and wells, back chips, sheets), the press tint, the glyph on
       an app tile, Drive's class colours; light = the light palette (apps then use the light tiles, icon_g1) */
    bool     light;
    uint32_t panel, panel_edge, inset, inset_hi, chip, label2, off, sheet, press_tint, tile_glyph, dcls[4];
    /* style B (Drive) */
    uint32_t b_ground, b_tile, b_border, b_shadow, b_lit, b_lit_hi, b_unlit, b_tank, b_face_tile, b_face_text, b_head,
             b_tag_model, b_tag_phys, b_tag_learn, b_text, b_muted;
    /* style C (home) */
    uint32_t c_bg, c_track, c_rpm, c_tick, c_status, c_btn, c_btn_glyph, c_text, c_muted;
    int16_t  radius_card, radius_row, radius_button, radius_tile_b, border_card, border_accent, pad_card, shadow_b;
    /* the Colour look (F): 2-stop vertical gradients, glass cards (white at card_opa, white border at border_opa) */
    bool     gradients;
    uint8_t  card_opa, border_opa;
    uint32_t glass, wall_top, wall_bot, wall_app_top[SEC_COUNT], wall_app_bot[SEC_COUNT];
    uint32_t econ_g1, econ_g2, cls_g1[4], cls_g2[4], end_g1, end_g2, face_top, face_bot, face_tick, face_dot, face_hour, face_min;
    uint32_t sb_cpu, sb_ai, sb_mips, sb_pwr;
    const lv_font_t *f_speed, *f_num_b, *f_hero, *f_big, *f_large, *f_title, *f_value, *f_status, *f_body, *f_label, *f_small;
} ui_theme_t;
extern const ui_theme_t *ui_th;                  /* the palette in force (dark or light) */
#define UI_THEME (*ui_th)
uint32_t ui_accent(void);                        /* accent of the app on screen (section colour) */

#define UI_BG        (UI_THEME.bg)
#define UI_CARD      (UI_THEME.card)
#define UI_BORDER    (UI_THEME.border)
#define UI_TEXT      (UI_THEME.text)
#define UI_MUTED     (UI_THEME.muted)
#define UI_ORANGE    (ui_accent())
#define UI_RED       (UI_THEME.bad)
#define UI_CRIMSON   (UI_THEME.ai)
#define UI_GREEN     (UI_THEME.good)
#define UI_WARN      (UI_THEME.warn)
#define UI_ICON      (UI_THEME.icon)
#define UI_LIGHT     (UI_THEME.light)
#define UI_MARGIN    16
#define UI_W         480
#define UI_H         480
#define UI_TOP       84          /* below the two-row status bar (lv_layer_top) */
#define UI_SB_H      80

typedef struct {
    app_status_t       *st;      /* status_get copy */
    ml_stats_t          ml;
    float               probs[4];
    int                 nprobs;
    rev_status_t        rs;
    mosaico_gauge_t     gauge;
    mosaico_storage_t   fat;
    int64_t             now_ms;
    char                clock[16];   /* "17:31" when the clock is set, else uptime "1:02:03" style "up 12m" */
    bool                obd_live;   /* adapter logging with engine data */
    bool                drive_active;   /* the OBD poll loop is in a drive (elm_drive_active) */
    int                 model;      /* ml_model_active(): the model in use, E.V or R.E.V; every model mark and name follows it */
} ui_snap_t;

typedef struct {
    const char *id;          /* NVS key part, stable */
    const char *name;        /* label under the icon */
    const char *glyph;       /* LV_SYMBOL_* or short text in the icon */
    void (*create)(lv_obj_t *parent);
    void (*update)(const ui_snap_t *s);
    void (*destroy)(void);
    uint16_t period_ms;
    uint8_t  section;        /* ui_section_t: the app's accent */
    uint32_t icon_bg;        /* launcher tile colour (E_Apps), the glyph is dark on it */
    uint32_t icon_g1, icon_g2;   /* icon_g1: the light-theme tile (white glyph on it, H_AppsLight); icon_g2 unused */
    const lv_image_dsc_t *icon52, *icon24;   /* Tabler outline icon, A8 (mosaico_icons.c); icon24 unused (NULL) */
    const lv_image_dsc_t *tile;              /* a full-colour mark that replaces the icon tile (a model mark), or NULL */
} ocm_app_t;

/* app_main calls this once ml_init() and rev_init() have run: before that the snapshot leaves the R.E.V figures empty */
void mosaico_ui_producers_ready(void);
uint32_t mosaico_touch_errors(void);            /* failed CST9220 reads (counted, never fatal) */
void ui_swipe_status(char *buf, size_t n);      /* CONFIG_OCM_SWIPE_INTENT: the Settings line (LVGL task) */

/* app registry (mosaico_apps.c) */
int               ui_app_count(void);
const ocm_app_t  *ui_app(int i);

/* settings, kept in NVS by the board task (the LVGL task never writes flash); the blob, its migration and the
   change rules are in ui_settings_logic.h (pure C, host-tested) */
#include "ui_settings_logic.h"
typedef enum { W_FUEL_LPH = 0, W_L100, W_KPL, W_SPEED, W_RPM, W_TORQUE, W_COUNT } ui_widget_t;
const char *ui_widget_name(int w);
void ui_settings_get(ui_settings_t *out);
/* applies in RAM at once; the NVS write (500 ms after the last change), a launcher rebuild (Home order) and the theme
   switch run later, debounced, outside the tap (ui_settings_logic.h). Nothing on the Settings page is rebuilt. */
void ui_settings_set(const ui_settings_t *in);
void ui_brightness_preview(int pct);             /* slider drag: panel only (<= 1 command per 50 ms UI tick), not saved */
void ui_settings_set_brightness(int pct);        /* slider release: 10..100 %, applied and saved, nothing rebuilt */

/* shell helpers for apps */
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t border);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *text);
lv_obj_t *ui_caption(lv_obj_t *parent, const char *upper_text);      /* small UPPERCASE orange label */
lv_obj_t *ui_pill(lv_obj_t *parent, const char *text, uint32_t color);
lv_obj_t *ui_header(lv_obj_t *parent, const char *title);           /* app title row with a round back chevron */
lv_obj_t *ui_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius);   /* plain filled box */
void      ui_set_bg(lv_obj_t *o, uint32_t c);                        /* change-only background colour */
void      ui_glass(lv_obj_t *o);                                     /* Colour theme: glass card; else nothing */
void      ui_grad(lv_obj_t *o, uint32_t top, uint32_t bottom);      /* 2-stop vertical gradient (Colour theme only) */
void      ui_wallpaper(lv_obj_t *o, int section);                   /* screen background: -1 = home and grid */
void      ui_set_text(lv_obj_t *o, const char *t);                   /* change-only */
void      ui_set_fmt(lv_obj_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void      ui_set_color(lv_obj_t *o, uint32_t c);
void      ui_go_home(void);
/* true while the shell rebuilds the open app for a theme switch: an app may keep its scroll position across it */
bool      ui_theme_reopening(void);
/* economy in the unit chosen in Settings: writes the number into buf ("--" when kpl < 0.05) and returns the unit */
const char *ui_econ(float kpl, char *buf, size_t n);
/* modal question on top of everything: only the yes button runs cb (never a single tap on the item itself) */
void      ui_confirm(const char *text, const char *yes, void (*cb)(void *arg), void *arg);
/* the End drive control (hold 1 s) / Start drive (tap), centred at (cx, y); refreshed by ui_drive_ctl_refresh */
lv_obj_t *ui_drive_ctl(lv_obj_t *parent, int cx, int y);
lv_obj_t *ui_drive_pill(lv_obj_t *parent, int x, int y, int w, int h);   /* the same control as a pill (Home, Drive header) */
void      ui_drive_ctl_refresh(lv_obj_t *ctl, const ui_snap_t *s);
void      ui_mark_jobs(uint32_t *runs, uint32_t *misses, uint32_t *max_us, uint32_t *late_max_ms);   /* the UI job's own figures */
const char *ui_class_name(int k);                                    /* "Stopped" ... "--" */
