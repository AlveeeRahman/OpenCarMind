/* ESP-Mosaico apps (mosaico_ui.h). Each app is one ocm_app_t in APPS[]: to add a panel, write create/update (and
 * destroy if it keeps pointers that must not dangle) and add one line to the table. Apps only read the ui_snap_t the
 * shell hands them; every value comes from a firmware data path (status, ML, the learners, gauge, sched_mon), and a
 * value the firmware does not have shows "--". Units are metric. */
#include "sdkconfig.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "mosaico_ui.h"
#include "econ.h"
#include "sched_mon.h"
#include "version.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <time.h>
#if CONFIG_OCM_NAND_LOGS
#include "nand_store.h"
#endif
#include "mosaico_files.h"
#include "mosaico_drive.h"
#include "fault_engine.h"
/* Model marks: every model mark and model name on the screens follows the model in use (ui_snap_t.model), E.V or R.E.V.
   The R.E.V and E.V apps keep their own tile and mark: each is the page of its model, whichever model runs, with the
   "Use" button that switches to it. */
extern const lv_image_dsc_t ocm_mark_rev_34, ocm_mark_rev_24, ocm_mark_rev_tile, ocm_mark_ev_34, ocm_mark_ev_24, ocm_mark_ev_tile;
/* Only R.E.V learns: the learned state on the board is R.E.V's whichever model runs (E.V keeps it as saved), so the Files
   and Format texts name R.E.V on both models. */
#define LEARNED_STATE "R.E.V"
#define LEARNED_STATE_AND "R.E.V"
extern const lv_image_dsc_t ocm_icon_drive_52, ocm_icon_ai_52, ocm_icon_learn_52, ocm_icon_summary_52, ocm_icon_live_52, ocm_icon_fuel_52, ocm_icon_brakes_52, ocm_icon_trip_52, ocm_icon_engine_52, ocm_icon_faults_52, ocm_icon_board_52, ocm_icon_files_52, ocm_icon_settings_52, ocm_icon_wifi_52;
#include "elm327.h"
#include "mosaico_clock.h"
#include "remote_config.h"
#include "ui_events_logic.h"
#include "ui_rows_logic.h"
#include "power_state.h"
#include "sdlog.h"
#include "sys_mode.h"
#include "uploader.h"

#define F48 (UI_THEME.f_hero)
#define F32 (UI_THEME.f_big)
#define F28 (UI_THEME.f_large)
#define F20 (UI_THEME.f_value)
#define F16 (UI_THEME.f_body)
#define F14 (UI_THEME.f_label)
#define CW  (UI_W - 2 * UI_MARGIN)          /* full card width */
#define HW  ((CW - 12) / 2)                 /* half card width */
#define Y0  (UI_TOP + 42)                   /* first card row */

/* ---------- small building blocks ---------- */
static lv_obj_t *at(lv_obj_t *o, int x, int y) { lv_obj_set_pos(o, x, y); return o; }

static lv_obj_t *bar(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int max)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, max);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static void bar_set(lv_obj_t *b, int v)
{
    if (lv_bar_get_value(b) != v) lv_bar_set_value(b, v, LV_ANIM_OFF);
}

/* a "name ........ value" row inside a card; returns the value label */
static lv_obj_t *row(lv_obj_t *card, int y, const char *name)
{
    at(ui_label(card, F16, UI_MUTED, name), 4, y);
    lv_obj_t *v = ui_label(card, F20, UI_TEXT, "--");
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -4, y - 2);
    return v;
}

static void pill_set(lv_obj_t *p, const char *t, uint32_t c)
{
    ui_set_text(p, t);
    ui_set_color(p, c);
    if (lv_color_to_u32(lv_obj_get_style_border_color(p, 0)) != lv_color_to_u32(lv_color_hex(c)))
        lv_obj_set_style_border_color(p, lv_color_hex(c), 0);
}

static bool rev_in_use(const ui_snap_t *s) { return s->model == ML_MODEL_REV; }
static const char *model_tag(const ui_snap_t *s) { return rev_in_use(s) ? "R.E.V" : "E.V"; }
static const lv_image_dsc_t *model_mark_34(const ui_snap_t *s) { return rev_in_use(s) ? &ocm_mark_rev_34 : &ocm_mark_ev_34; }
static void mark_set(lv_obj_t *img, const lv_image_dsc_t *mk) { if (lv_image_get_src(img) != (const void *)mk) lv_image_set_src(img, mk); }
static bool have_f(const ui_snap_t *s, int f) { return s->obd_live && OBD_HAS(&s->st->obd, f); }
/* Where the torque figure comes from, the same rule on Drive, AI and Brakes: the physics torque (from fuel and rpm) while
   the car reports it, else the model's estimate. The badge shows it on every update, also before the first value. */
static bool torque_physics(const ui_snap_t *s) { return s->obd_live && isfinite(s->st->obd.torque_nm); }
#define TORQUE_SRC(s) (torque_physics(s) ? "PHYSICS" : "MODEL")
static float fv(const ui_snap_t *s, int f) { return s->st->obd.f[f]; }
static void set_int_or_dash(lv_obj_t *o, bool ok, float v) { if (ok) ui_set_fmt(o, "%d", (int)lroundf(v)); else ui_set_text(o, "--"); }
static int pct(float p) { return (int)lroundf(p * 100.0f); }

/* ======================= D pages: one layout per app, from the owner's artboards (design canvas row D) =====================
 * Common grid: 20 px side margin, 30 px header row at y 18 (round chevron, title), content from y 58 to 464, 10 px gaps.
 * Cards #0B0B0D with a 1 px border (radius 18); hero cards 2 px in the page accent (radius 22). Labels: the theme's small
 * font, letter-spaced, muted. Numbers: f_num_b (56 px) for heroes, f_hero (48) for 42 px designs, then 28/24/20. */
#define DX   20
#define DW   440
#define DHY  (UI_TOP)          /* header row under the status bar */
#define DY   (UI_TOP + 38)     /* content from here to 464 */

/* Back geometry (ui_back): each touch area ends above its page's first control, each chevron sits inside its area and
   below the status bar */
_Static_assert(UI_BACK_HIT_W >= 96 && DY >= 96 && UI_TOP + 40 >= 96, "Back touch area under 96 x 96");
_Static_assert(DHY - 7 >= UI_SB_APP_BOTTOM && DHY - 7 + UI_BACK_D <= DY, "D-page chevron outside its row");
_Static_assert(UI_SB_APP_BOTTOM + 3 + UI_BACK_D <= UI_TOP + 40 && UI_TOP + 40 < Y0, "ui_header Back overlaps the list");

static lv_obj_t *d_card(lv_obj_t *p, int x, int y, int w, int h, uint32_t border, bool hero)
{
    lv_obj_t *c = ui_rect(p, x, y, w, h, UI_CARD, hero ? 22 : 18);
    lv_obj_set_style_border_width(c, hero ? 2 : 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(border), 0);
    lv_obj_set_style_pad_hor(c, hero ? 16 : 14, 0);
    lv_obj_set_style_pad_ver(c, 12, 0);
    if (UI_THEME.gradients) {                            /* Colour: glass; hero cards keep their accent border */
        ui_glass(c);
        if (hero) { lv_obj_set_style_border_color(c, lv_color_hex(border), 0); lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0); }
    }
    return c;
}

static lv_obj_t *d_lab(lv_obj_t *p, const char *t, int x, int y, uint32_t c)
{
    lv_obj_t *l = ui_label(p, UI_THEME.f_small, c, t);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *d_txt(lv_obj_t *p, const lv_font_t *f, uint32_t c, const char *t, int x, int y)
{
    lv_obj_t *l = ui_label(p, f, c, t);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* value with a small unit after it, baseline-ish aligned */
typedef struct { lv_obj_t *v, *u; } d_val_t;
static d_val_t d_value(lv_obj_t *p, const lv_font_t *f, const char *unit, int x, int y)
{
    d_val_t r;
    r.v = d_txt(p, f, UI_TEXT, "--", x, y);
    r.u = ui_label(p, UI_THEME.f_label, UI_MUTED, unit);
    lv_obj_align_to(r.u, r.v, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -(lv_font_get_line_height(f) / 8));
    return r;
}
static void d_value_set(d_val_t *d, const char *t)
{
    ui_set_text(d->v, t);
    lv_obj_update_layout(d->v);
    lv_obj_align_to(d->u, d->v, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -(lv_font_get_line_height(lv_obj_get_style_text_font(d->v, 0)) / 8));
}

/* header: a flex row at the top of the page. Back (ui_back: a 44 px chevron, a 104 x 122 px touch area from the corner
   down to the content), title, items placed after the title (d_after), a spacer, items on the right (d_right). No pixel
   offsets between texts. */
static lv_obj_t *s_hr, *s_hr_sp;
static lv_obj_t *d_header(lv_obj_t *p, const char *title)
{
    ui_back(p, DX, DHY - 7, DY, UI_THEME.chip, UI_ORANGE, NULL);
    s_hr = lv_obj_create(p);
    lv_obj_remove_style_all(s_hr);
    lv_obj_set_pos(s_hr, DX, DHY - 7);
    lv_obj_set_size(s_hr, DW, UI_BACK_D);
    lv_obj_set_flex_flow(s_hr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_hr, 10, 0);
    lv_obj_remove_flag(s_hr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_hr, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *b = lv_obj_create(s_hr);                    /* the chevron's place in the row (it is drawn by ui_back) */
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, UI_BACK_D, 1);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = ui_label(s_hr, UI_THEME.f_value, UI_TEXT, title);
    s_hr_sp = lv_obj_create(s_hr);
    lv_obj_remove_style_all(s_hr_sp);
    lv_obj_set_size(s_hr_sp, 1, 1);
    lv_obj_set_flex_grow(s_hr_sp, 1);
    lv_obj_remove_flag(s_hr_sp, LV_OBJ_FLAG_CLICKABLE);
    return t;
}
static lv_obj_t *d_right(lv_obj_t *p, const char *t, uint32_t c)
{
    (void)p;
    return ui_label(s_hr, UI_THEME.f_label, c, t);
}
static lv_obj_t *d_after(lv_obj_t *anchor, lv_obj_t *o)
{
    (void)anchor;
    lv_obj_set_parent(o, s_hr);
    lv_obj_move_to_index(o, (int32_t)lv_obj_get_index(s_hr_sp));
    return o;
}
/* put o in the header just before the right-hand item r (a status dot before its text) */
static lv_obj_t *d_before(lv_obj_t *r, lv_obj_t *o)
{
    lv_obj_set_parent(o, s_hr);
    lv_obj_move_to_index(o, (int32_t)lv_obj_get_index(r));
    return o;
}
static lv_obj_t *d_dot(lv_obj_t *p, uint32_t c) { return ui_rect(p, 0, 0, 8, 8, c, LV_RADIUS_CIRCLE); }
static lv_obj_t *d_bar(lv_obj_t *p, int x, int y, int w, int h, uint32_t c, int max)
{
    lv_obj_t *b = bar(p, x, y, w, h, c, max);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
    return b;
}
static uint32_t d_cls_color(int k) { return k >= 0 && k < 4 ? UI_THEME.cls[k] : UI_THEME.dim; }

/* ======================= AI ======================= */
static lv_obj_t *a_mark, *a_hero, *a_tag, *a_right, *a_cls, *a_sure, *a_pb[4], *a_pp[4], *a_note, *a_brk_bar, *a_tq_tag, *a_fu_tag;
static d_val_t a_brk, a_tq, a_fu;

static void ai_create(lv_obj_t *p)
{
    lv_obj_t *t = d_header(p, "AI");
    bool rev = ml_model_active() == ML_MODEL_REV;           /* the first frame already names the model in use */
    a_tag = d_after(t, ui_pill(p, rev ? "R.E.V" : "E.V", UI_ORANGE));
    a_mark = d_after(t, lv_image_create(p));
    lv_image_set_src(a_mark, rev ? &ocm_mark_rev_34 : &ocm_mark_ev_34);
    a_right = d_right(p, "", UI_MUTED);
    lv_obj_t *c = a_hero = d_card(p, DX, DY, DW, 168, UI_ORANGE, true);
    d_lab(c, "AI SEES", 0, 10, UI_ORANGE);
    a_cls = d_txt(c, UI_THEME.f_large, UI_TEXT, "--", 80, 0);
    a_sure = ui_label(c, UI_THEME.f_body, UI_TEXT, "");
    lv_obj_align(a_sure, LV_ALIGN_TOP_RIGHT, 0, 6);
    static const char *const L[4] = {"Stopped", "Braking", "Accelerating", "Cruising"};
    for (int k = 0; k < 4; k++) {
        d_txt(c, UI_THEME.f_label, UI_MUTED, L[k], 0, 38 + k * 23);
        a_pb[k] = d_bar(c, 100, 44 + k * 23, 250, 8, UI_THEME.dim, 100);
        a_pp[k] = ui_label(c, UI_THEME.f_label, UI_TEXT, "");
        lv_obj_align(a_pp[k], LV_ALIGN_TOP_RIGHT, 0, 38 + k * 23);
    }
    a_note = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 128);
    c = d_card(p, DX, DY + 176, DW, 166, UI_CRIMSON, true);
    d_lab(c, "AI ESTIMATES", 0, 0, UI_CRIMSON);
    d_txt(c, UI_THEME.f_body, UI_TEXT, "Brake pressure", 0, 30);
    a_brk_bar = d_bar(c, 130, 37, 150, 6, UI_CRIMSON, 100);
    a_brk = d_value(c, UI_THEME.f_title, "bar", 300, 22);
    d_txt(c, UI_THEME.f_body, UI_TEXT, "Engine torque", 0, 74);
    a_tq_tag = at(ui_pill(c, "MODEL", UI_CRIMSON), 130, 76);
    a_tq = d_value(c, UI_THEME.f_title, "N m", 300, 66);
    d_txt(c, UI_THEME.f_body, UI_TEXT, "Fuel rate", 0, 118);
    a_fu_tag = at(ui_pill(c, "LEARNED", UI_GREEN), 130, 120);
    a_fu = d_value(c, UI_THEME.f_title, "L/h", 300, 110);
}

static void ai_update(const ui_snap_t *s)
{
    const ml_stats_t *m = &s->ml;
    pill_set(a_tag, model_tag(s), UI_ORANGE);
    mark_set(a_mark, model_mark_34(s));
    if (m->windows) ui_set_fmt(a_right, "%lu windows, %.1f ms", (unsigned long)m->windows, m->last_us / 1000.0);
    else ui_set_text(a_right, m->ready ? "no windows yet" : "model not loaded");
    bool have = m->last_class >= 0;
    ui_set_text(a_cls, have ? ui_class_name(m->last_class) : "--");
    if (UI_THEME.gradients) {                             /* Colour: the AI SEES card takes the class gradient */
        int k = have ? m->last_class : 3;
        ui_grad(a_hero, UI_THEME.cls_g1[k], UI_THEME.cls_g2[k]);
        lv_obj_set_style_border_width(a_hero, 0, 0);
    }
    ui_set_color(a_cls, have ? UI_TEXT : UI_MUTED);
    if (have) ui_set_fmt(a_sure, "%d %% sure", pct(m->last_prob)); else ui_set_text(a_sure, "");
    for (int k = 0; k < 4; k++) {
        int v = s->nprobs > k ? pct(s->probs[k]) : 0;
        bar_set(a_pb[k], v);
        lv_obj_set_style_bg_color(a_pb[k], lv_color_hex(have && k == m->last_class ? d_cls_color(k) : UI_THEME.dim), LV_PART_INDICATOR);
        if (s->nprobs > k) ui_set_fmt(a_pp[k], "%d %%", v); else ui_set_text(a_pp[k], "--");
    }
    if (have && m->last_at_ms) ui_set_fmt(a_note, rev_in_use(s) ? "Last window %lld s ago. The network is frozen, learning runs beside it."
                                                                : "Last window %lld s ago. E.V runs as shipped, nothing learns.",
                                          (long long)((s->now_ms - m->last_at_ms) / 1000));
    else ui_set_text(a_note, "Waits for a drive: one answer per window.");
    char b[16];
    if (have) { snprintf(b, sizeof b, "%.1f", (double)m->last_est[0]); bar_set(a_brk_bar, (int)fminf(m->last_est[0] * 5, 100)); }
    else { snprintf(b, sizeof b, "--"); bar_set(a_brk_bar, 0); }
    d_value_set(&a_brk, b);
    const obd_values_t *o = &s->st->obd;
    bool tq = torque_physics(s);
    pill_set(a_tq_tag, TORQUE_SRC(s), tq ? UI_THEME.estimate : UI_CRIMSON);
    if (tq) snprintf(b, sizeof b, "%.0f", (double)o->torque_nm);
    else if (have) snprintf(b, sizeof b, "%.0f", (double)m->last_est[1]);
    else snprintf(b, sizeof b, "--");
    d_value_set(&a_tq, b);
    ui_set_color(a_tq.v, UI_THEME.estimate);
    /* only R.E.V learns: with E.V in use the tag says MODEL even before the first window (the S3 rule, display.c) */
    bool lrn = m->learned_ok && rev_in_use(s);
    if (lrn) { snprintf(b, sizeof b, "%.1f", (double)m->learned_lph); pill_set(a_fu_tag, "LEARNED", UI_GREEN); }
    else if (have) { snprintf(b, sizeof b, "%.1f", (double)m->last_est[2]); pill_set(a_fu_tag, "MODEL", UI_CRIMSON); }
    else { snprintf(b, sizeof b, "--"); pill_set(a_fu_tag, rev_in_use(s) ? "LEARNED" : "MODEL", rev_in_use(s) ? UI_GREEN : UI_CRIMSON); }
    d_value_set(&a_fu, b);
    ui_set_color(a_fu.v, lrn ? UI_GREEN : UI_TEXT);
}

/* ======================= Learning ======================= */
static lv_obj_t *l_saved_dot, *l_saved, *l_f2, *l_f3, *l_k2, *l_k3, *l_tank_u, *l_tbar, *l_tnote, *l_state, *l_n[3], *l_mark, *l_note;
static lv_obj_t *l_card[3];                                /* fuel read-out, airflow constant, tank: dimmed while E.V runs */
static d_val_t l_f1, l_k1, l_t1;

/* the self-test of the model at load: its outputs on the file's own test input against ESP-PPQ's PC simulation */
static const char *selftest_word(void)
{
    int t = ml_selftest_state();
    return t > 0 ? "exact" : t == 0 ? "differs" : "--";
}

static void learn_create(lv_obj_t *p)
{
    d_header(p, "Learning");
    l_saved = d_right(p, "", UI_GREEN);
    l_saved_dot = d_before(l_saved, d_dot(p, UI_GREEN));
    int hw = (DW - 8) / 2;
    lv_obj_t *c = l_card[0] = d_card(p, DX, DY, hw, 96, UI_BORDER, false);
    d_lab(c, "FUEL READ-OUT", 0, 0, UI_MUTED);
    l_f1 = d_value(c, UI_THEME.f_large, "updates kept", 0, 14);
    l_f2 = d_txt(c, UI_THEME.f_label, UI_GREEN, "", 0, 46);
    l_f3 = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 62);
    c = l_card[1] = d_card(p, DX + hw + 8, DY, hw, 96, UI_BORDER, false);
    d_lab(c, "AIRFLOW CONSTANT", 0, 0, UI_MUTED);
    l_k1 = d_value(c, UI_THEME.f_large, "", 0, 14);
    l_k2 = d_txt(c, UI_THEME.f_label, UI_TEXT, "", 0, 46);
    l_k3 = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 62);
    c = l_card[2] = d_card(p, DX, DY + 104, DW, 80, UI_BORDER, false);
    d_lab(c, "FUEL TANK", 0, 4, UI_MUTED);
    l_t1 = d_value(c, UI_THEME.f_value, "", 150, 0);
    l_tank_u = l_t1.u;
    l_tbar = d_bar(c, 0, 28, DW - 28, 8, UI_GREEN, 100);
    l_tnote = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 42);
    c = d_card(p, DX, DY + 192, DW, 150, UI_GREEN, false);
    lv_obj_t *mk = l_mark = lv_image_create(c);           /* the mark of the model in use next to its figures */
    lv_image_set_src(mk, ml_model_active() == ML_MODEL_REV ? &ocm_mark_rev_24 : &ocm_mark_ev_24);
    lv_obj_set_pos(mk, 0, -4);
    d_lab(c, "MODEL", 30, 0, UI_GREEN);
    l_state = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    lv_obj_align(l_state, LV_ALIGN_TOP_RIGHT, 0, 0);
    static const char *const N[3] = {"windows read", "self-test", "one run"};
    for (int k = 0; k < 3; k++) {
        l_n[k] = d_txt(c, UI_THEME.f_title, UI_TEXT, "--", k * 140, 26);
        d_txt(c, UI_THEME.f_small, UI_MUTED, N[k], k * 140, 58);
    }
    lv_obj_t *n = l_note = d_txt(c, UI_THEME.f_label, UI_MUTED, "", 0, 86);
    lv_obj_set_width(n, DW - 28);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
}

static void learn_update(const ui_snap_t *s)
{
    const rev_status_t *r = &s->rs;
    /* E.V in use: R.E.V's learner is paused (rev_model_changed), its figures below are the values it keeps */
    bool off = !rev_in_use(s);
    ui_set_text(l_saved, off ? "Learning off: E.V in use" : r->saved ? "Saved on the board" : r->enabled ? "Not saved yet" : "Learning off");
    ui_set_color(l_saved, off ? UI_WARN : r->saved ? UI_GREEN : UI_MUTED);
    ui_set_bg(l_saved_dot, off ? UI_WARN : r->saved ? UI_GREEN : UI_THEME.dim);
    for (int k = 0; k < 3; k++)
        if (lv_obj_get_style_opa(l_card[k], 0) != (off ? LV_OPA_40 : LV_OPA_COVER)) lv_obj_set_style_opa(l_card[k], off ? LV_OPA_40 : LV_OPA_COVER, 0);
    char b[24];
    snprintf(b, sizeof b, "%lu", (unsigned long)r->kept);
    d_value_set(&l_f1, b);
    if (r->n_err[0] && r->n_err[1]) ui_set_fmt(l_f2, "%.2f vs %.2f L/h error", sqrt(r->err[1] / r->n_err[1]), sqrt(r->err[0] / r->n_err[0]));
    else ui_set_text(l_f2, "error: needs labelled rows");
    ui_set_fmt(l_f3, "learned vs factory, %lu pairs", (unsigned long)r->pairs);
    if (r->k_ready) { if (r->k < 0.01f) snprintf(b, sizeof b, "%.2f", (double)(r->k * 1e4f)); else snprintf(b, sizeof b, "%.3f", (double)r->k); }
    else snprintf(b, sizeof b, "--");
    d_value_set(&l_k1, b);
    ui_set_text(l_k1.u, r->k_ready && r->k < 0.01f ? "e-4" : "");
    if (r->vd_l > 0) ui_set_fmt(l_k2, "engine %.1f L", (double)r->vd_l); else ui_set_text(l_k2, "engine size: learning");
    ui_set_fmt(l_k3, "from %lu rows", (unsigned long)r->k_rows);
    if (r->tank.have) snprintf(b, sizeof b, "%.0f L", (double)(r->tank.cap_raw_l * r->cal)); else snprintf(b, sizeof b, "--");
    d_value_set(&l_t1, b);
    ui_set_fmt(l_tank_u, " learned from %lu fill-up%s", (unsigned long)r->tank_fills, r->tank_fills == 1 ? "" : "s");
    bar_set(l_tbar, (int)fminf(fmaxf(r->tank.span_pct, 0), 100));
    ui_set_fmt(l_tnote, "Gauge span seen: %.0f %%. More fill-ups make it sharper.", (double)r->tank.span_pct);
    const ml_stats_t *m = &s->ml;
    mark_set(l_mark, rev_in_use(s) ? &ocm_mark_rev_24 : &ocm_mark_ev_24);
    if (m->ready) ui_set_fmt(l_state, "%s %s", model_tag(s), ml_model_version(s->model)); else ui_set_fmt(l_state, "%s not loaded", model_tag(s));
    ui_set_text(l_note, rev_in_use(s) ? "The classifier stays as shipped, the fuel read-out learns in this car."
                                      : "Learning is off while E.V runs. R.E.V keeps what it learned. Use R.E.V turns it on again.");
    ui_set_color(l_note, off ? UI_WARN : UI_MUTED);
    ui_set_fmt(l_n[0], "%lu", (unsigned long)m->windows);
    ui_set_text(l_n[1], m->ready ? selftest_word() : "--");
    if (m->ready && m->selftest_us) ui_set_fmt(l_n[2], "%.1f ms", m->selftest_us / 1000.0); else ui_set_text(l_n[2], "--");
}

/* ======================= R.E.V and E.V: one app per model (M1) =======================
 * Each model app shows its model, its self-test and a 64 px "Use" button. The button sends the Settings > MODEL request
 * (model_pick: the board task's ml_select_model, the USB door's MODEL SET path; refused while a drive runs) and reads
 * "In use" while its model runs. R.E.V adds its fuel learning, E.V what it answers. One mp_t per model: the shell can
 * build one model app while the other's objects still wait for their deferred delete. */

/* the model request, shared by the Settings segment and both "Use" buttons; the board task answers in
   mosaico_model_result_t */
static int g_model_req = -1;                               /* requested id (-1 none) */
static uint32_t g_model_gen;                               /* mosaico_model_result_t.gen when the request was sent */
static char g_model_err[80];
static bool g_model_dialog;                                /* sent by a "Use" button: a refusal opens a dialog */

/* mossREV OS: every "not now" sheet comes from the runlevel lookup (os_core.h). A drive keeps the sheets' old text. */
static bool os_refuse_sheet(os_svc_t svc, const char *action)
{
    EXT_RAM_BSS_ATTR static char t[112];             /* the sheet copies it: PSRAM is fine */
    const char *no = os_refusal(svc);
    if (!no) return false;
    if (os_blocker(svc) == OS_RL_DRIVING && elm_drive_active()) snprintf(t, sizeof t, "A drive is active. End the drive first, then %s.", action);
    else snprintf(t, sizeof t, "Not now: %s. Try again later.", no);
    ui_confirm(t, "OK", NULL, NULL);
    return true;
}

static void model_pick(int id)
{
    if (g_model_req >= 0 || id == ml_model_active()) return;
    if (!ml_model_available(id)) { ui_confirm(id == ML_MODEL_EV ? "CarMind E.V is not in this firmware." : "CarMind R.E.V is not in this firmware.", "OK", NULL, NULL); return; }
    if (os_refuse_sheet(OS_SVC_MODEL_SWITCH, "switch models")) return;
    mosaico_model_result_t r;
    mosaico_board_model_result(&r);
    if (!mosaico_board_request(MB_MODEL_SET, (uint32_t)id)) { ui_confirm("The board is busy. Try again in a moment.", "OK", NULL, NULL); return; }
    g_model_gen = r.gen;
    g_model_req = id;
    g_model_err[0] = 0;
    g_model_dialog = false;
}

/* dialog: the caller is a model app (Settings shows the refusal in its MODEL line instead) */
static void model_poll(bool dialog)
{
    if (g_model_req < 0) return;
    mosaico_model_result_t r;
    mosaico_board_model_result(&r);
    if (r.gen == g_model_gen) return;                      /* the board task has not answered yet */
    if (!r.ok) snprintf(g_model_err, sizeof g_model_err, "%s", r.why);
    g_model_req = -1;
    if (!r.ok && dialog && g_model_dialog) {
        char q[112];
        snprintf(q, sizeof q, "Not switched: %s.", r.why);
        ui_confirm(q, "OK", NULL, NULL);
        g_model_err[0] = 0;                                /* shown once */
    }
}

/* page grid under the header: the MODEL and SELF-TEST cards, a status line, the model's own card, the Use button */
#define MP_CARDS_H 100
#define MP_LINE_Y  (DY + MP_CARDS_H + 6)
#define MP_LINE_H  40
#define MP_CARD_Y  (MP_LINE_Y + MP_LINE_H + 6)
#define MP_CARD_H  118
#define MP_USE_Y   (MP_CARD_Y + MP_CARD_H + 8)
#define MP_USE_H   64
_Static_assert(MP_USE_Y + MP_USE_H <= UI_H - UI_MARGIN, "the Use button runs off the page");

enum { MP_USE_IN_USE = 0, MP_USE_OFFER, MP_USE_LOADING };
typedef struct {
    lv_obj_t *sub, *step, *card[2], *big[2], *l1[2], *l2[2], *ok, *line, *right, *n[3], *note, *note_r, *use, *use_lbl;
    int use_state;                                         /* painted MP_USE_*, -1 before the first paint */
} mp_t;
static mp_t g_mp[ML_MODEL_COUNT];

static const char *mp_tag(int id) { return id == ML_MODEL_REV ? "R.E.V" : "E.V"; }

static void mp_use_paint(int id, int active)
{
    mp_t *v = &g_mp[id];
    int st = g_model_req == id ? MP_USE_LOADING : active == id ? MP_USE_IN_USE : MP_USE_OFFER;
    if (st == v->use_state || !v->use) return;
    v->use_state = st;
    uint32_t bg = st == MP_USE_OFFER ? UI_ORANGE : UI_THEME.track, edge = st == MP_USE_IN_USE ? UI_GREEN : st == MP_USE_OFFER ? UI_ORANGE : UI_THEME.dim;
    lv_obj_set_style_bg_color(v->use, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(v->use, st == MP_USE_OFFER ? lv_color_mix(lv_color_hex(UI_THEME.press_tint), lv_color_hex(UI_ORANGE), 64)
                                                          : lv_color_hex(bg), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(v->use, lv_color_hex(edge), 0);
    if (st == MP_USE_IN_USE) { ui_set_text(v->use_lbl, LV_SYMBOL_OK "  In use"); ui_set_color(v->use_lbl, UI_GREEN); }
    else if (st == MP_USE_OFFER) { ui_set_fmt(v->use_lbl, "Use %s", mp_tag(id)); ui_set_color(v->use_lbl, UI_BG); }
    else { ui_set_fmt(v->use_lbl, "Loading CarMind %s ...", mp_tag(id)); ui_set_color(v->use_lbl, UI_MUTED); }
}

static void mp_use_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    model_pick(id);                                        /* "In use": nothing to do; a drive: the dialog */
    if (g_model_req == id) g_model_dialog = true;
    mp_use_paint(id, ml_model_active());                   /* the tap shows at once, not on the next 1 s update */
}

static void mp_create(lv_obj_t *p, int id, const char *card_title, const char *const n[3])
{
    mp_t *v = &g_mp[id];
    bool rev = id == ML_MODEL_REV;
    v->use_state = -1;
    lv_obj_t *t = d_header(p, mp_tag(id));
    lv_obj_t *mk = d_after(t, lv_image_create(p));
    lv_image_set_src(mk, rev ? &ocm_mark_rev_34 : &ocm_mark_ev_34);
    v->sub = d_after(mk, ui_label(p, UI_THEME.f_label, UI_MUTED, ""));
    v->step = d_right(p, "", UI_MUTED);
    int hw = (DW - 8) / 2;
    static const char *const R[2] = {"MODEL", "SELF-TEST"};
    for (int k = 0; k < 2; k++) {
        lv_obj_t *c = v->card[k] = d_card(p, DX + k * (hw + 8), DY, hw, MP_CARDS_H, UI_THEME.dim, true);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_pad_ver(c, 10, 0);
        d_lab(c, R[k], 0, 0, UI_MUTED);
        v->big[k] = d_txt(c, UI_THEME.f_title, UI_TEXT, "--", 0, 15);
        v->l1[k] = d_txt(c, UI_THEME.f_label, UI_TEXT, "", 0, 42);
        v->l2[k] = d_txt(c, UI_THEME.f_label, UI_MUTED, "", 0, 59);
    }
    lv_obj_t *c = d_card(p, DX, MP_LINE_Y, DW, MP_LINE_H, UI_BORDER, false);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_ver(c, 0, 0);
    lv_obj_t *ok = v->ok = ui_label(c, UI_THEME.f_value, UI_GREEN, LV_SYMBOL_OK);
    lv_obj_align(ok, LV_ALIGN_LEFT_MID, 0, 0);
    v->line = ui_label(c, UI_THEME.f_body, UI_TEXT, "");
    lv_obj_align(v->line, LV_ALIGN_LEFT_MID, 30, 0);
    v->right = ui_label(c, UI_THEME.f_label, UI_MUTED, "");
    lv_obj_align(v->right, LV_ALIGN_RIGHT_MID, 0, 0);
    c = d_card(p, DX, MP_CARD_Y, DW, MP_CARD_H, UI_BORDER, false);
    d_lab(c, card_title, 0, 0, UI_ORANGE);
    for (int k = 0; k < 3; k++) {
        v->n[k] = d_txt(c, UI_THEME.f_large, UI_TEXT, "--", k * 140, 20);
        d_txt(c, UI_THEME.f_small, UI_MUTED, n[k], k * 140, 52);
    }
    v->note = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 74);
    v->note_r = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    lv_obj_align(v->note_r, LV_ALIGN_TOP_RIGHT, 0, 74);
    lv_obj_t *b = v->use = lv_button_create(p);
    lv_obj_set_pos(b, DX, MP_USE_Y);
    lv_obj_set_size(b, DW, MP_USE_H);
    lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
    lv_obj_set_style_border_width(b, UI_THEME.border_accent, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, mp_use_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    v->use_lbl = ui_label(b, UI_THEME.f_value, UI_TEXT, "");
    lv_obj_center(v->use_lbl);
    mp_use_paint(id, ml_model_active());
}

/* the part both model pages share; returns true while this page's model runs */
static bool mp_update(const ui_snap_t *s, int id)
{
    mp_t *v = &g_mp[id];
    const ml_stats_t *m = &s->ml;
    const char *other = mp_tag(id == ML_MODEL_REV ? ML_MODEL_EV : ML_MODEL_REV);
    model_poll(true);
    bool on = s->model == id, ready = on && m->ready;
    ui_set_text(v->sub, on ? "in use" : "not active");
    ui_set_color(v->sub, on ? UI_MUTED : UI_WARN);
    if (!on) ui_set_fmt(v->step, "%s in use", other);
    else if (m->windows) ui_set_fmt(v->step, "%lu windows, %.1f ms", (unsigned long)m->windows, m->last_us / 1000.0);
    else ui_set_text(v->step, m->ready ? "no windows yet" : "not loaded");
    int t = ready ? ml_selftest_state() : -1;
    uint32_t edge[2] = {ready ? UI_GREEN : on ? UI_THEME.dim : UI_WARN, t > 0 ? UI_GREEN : t == 0 ? UI_WARN : UI_THEME.dim};
    for (int k = 0; k < 2; k++)
        if (lv_color_to_u32(lv_obj_get_style_border_color(v->card[k], 0)) != lv_color_to_u32(lv_color_hex(edge[k])))
            lv_obj_set_style_border_color(v->card[k], lv_color_hex(edge[k]), 0);
    ui_set_text(v->big[0], ready ? ml_model_name(id) : on ? "--" : "Not active");
    ui_set_color(v->big[0], on ? UI_TEXT : UI_WARN);
    ui_set_fmt(v->l1[0], "model %s", ml_model_version(id));
    if (on) ui_set_fmt(v->l2[0], "for %s, %s", ml_model_target(id), m->ready ? "loaded" : "not loaded");
    else ui_set_fmt(v->l2[0], "%s runs now", other);
    ui_set_text(v->big[1], ready ? selftest_word() : "--");
    if (ready && m->selftest_us) ui_set_fmt(v->l1[1], "one run %.1f ms", m->selftest_us / 1000.0); else ui_set_text(v->l1[1], "");
    if (t >= 0) ui_set_text(v->l2[1], "vs PC simulation, 1 LSB");
    else if (on) ui_set_text(v->l2[1], "");
    else ui_set_fmt(v->l2[1], "runs when %s loads", mp_tag(id));
    ui_set_text(v->ok, on ? LV_SYMBOL_OK : LV_SYMBOL_PAUSE);
    ui_set_color(v->ok, on ? UI_GREEN : UI_WARN);
    mp_use_paint(id, s->model);
    return on;
}

/* R.E.V: the model, its self-test and the fuel learning it does in this car */
static void rev_create(lv_obj_t *p)
{
    static const char *const N[3] = {"updates taken", "kept as is", "pairs seen"};   /* rev_learn.c: commits, kept, pairs */
    mp_create(p, ML_MODEL_REV, "FUEL LEARNING IN THIS CAR", N);
}

static void rev_update(const ui_snap_t *s)
{
    mp_t *v = &g_mp[ML_MODEL_REV];
    const rev_status_t *r = &s->rs;
    /* E.V in use: R.E.V is not loaded, its learning is paused and everything it learned stays as saved */
    bool on = mp_update(s, ML_MODEL_REV);
    if (on) ui_set_text(v->line, r->learning ? "Learning fuel use in this car" : r->safe ? "Learning off: safe mode" : "Learning off");
    else ui_set_text(v->line, "Not active: learned values are kept");
    ui_set_text(v->right, !on ? "" : s->ml.z_ok ? "z output yes" : "z output no");
    ui_set_fmt(v->n[0], "%lu", (unsigned long)r->commits);
    ui_set_fmt(v->n[1], "%lu", (unsigned long)r->kept);
    ui_set_fmt(v->n[2], "%lu", (unsigned long)r->pairs);
    ui_set_text(v->note, r->saved ? "Saved on the board" : r->enabled ? "Not saved yet" : "Learning off");
    ui_set_fmt(v->note_r, "%lu reset%s to factory", (unsigned long)r->resets, r->resets == 1 ? "" : "s");
}

/* E.V: the model, its self-test and what it answers; it runs as shipped and never learns */
static void ev_create(lv_obj_t *p)
{
    static const char *const N[3] = {"windows read", "since last window", "one run"};
    mp_create(p, ML_MODEL_EV, "WHAT E.V ANSWERS", N);
}

static void ev_update(const ui_snap_t *s)
{
    mp_t *v = &g_mp[ML_MODEL_EV];
    const ml_stats_t *m = &s->ml;
    bool on = mp_update(s, ML_MODEL_EV);
    ui_set_text(v->line, on ? "Runs as shipped, no learning" : "Not active");
    ui_set_text(v->right, on ? "fixed weights" : "");
    if (on) ui_set_fmt(v->n[0], "%lu", (unsigned long)m->windows); else ui_set_text(v->n[0], "--");
    if (on && m->last_class >= 0 && m->last_at_ms) ui_set_fmt(v->n[1], "%lld s", (long long)((s->now_ms - m->last_at_ms) / 1000));
    else ui_set_text(v->n[1], "--");
    if (on && m->windows) ui_set_fmt(v->n[2], "%.1f ms", m->last_us / 1000.0); else ui_set_text(v->n[2], "--");
    ui_set_text(v->note, on ? "R.E.V's learning pauses, its learned values stay saved." : "R.E.V runs now. These figures start when E.V loads.");
    ui_set_text(v->note_r, "");
}

/* ======================= Live ======================= */
static d_val_t lv_spd, lv_rpm;
static lv_obj_t *lv_hz, *lv_b[5], *lv_v[5];
static uint32_t lv_rows0;
static int64_t lv_t0;

static void live_create(lv_obj_t *p)
{
    lv_obj_t *t = d_header(p, "Live");
    d_after(t, ui_label(p, UI_THEME.f_label, UI_MUTED, "straight from the car"));
    lv_hz = d_right(p, "", UI_GREEN);
    int hw = (DW - 8) / 2;
    lv_obj_t *c = d_card(p, DX, DY, hw, 96, UI_BORDER, false);
    d_lab(c, "SPEED", 2, 0, UI_MUTED);
    lv_spd = d_value(c, UI_THEME.f_hero, "km/h", 0, 14);
    c = d_card(p, DX + hw + 8, DY, hw, 96, UI_BORDER, false);
    d_lab(c, "ENGINE", 2, 0, UI_MUTED);
    lv_rpm = d_value(c, UI_THEME.f_hero, "rpm", 0, 14);
    c = d_card(p, DX, DY + 104, DW, 238, UI_BORDER, false);
    static const char *const L[5] = {"Throttle", "Engine load", "Coolant", "Fuel level", "Adapter"};
    for (int k = 0; k < 5; k++) {
        d_txt(c, UI_THEME.f_body, UI_TEXT, L[k], 0, 6 + k * 43);
        lv_b[k] = d_bar(c, 116, 12 + k * 43, 200, 10, k < 2 ? UI_ORANGE : UI_GREEN, 100);
        lv_v[k] = ui_label(c, UI_THEME.f_value, UI_TEXT, "--");
        lv_obj_align(lv_v[k], LV_ALIGN_TOP_RIGHT, 0, 2 + k * 43);
    }
    lv_rows0 = 0; lv_t0 = 0;
}

static void live_update(const ui_snap_t *s)
{
    char b[16];
    if (have_f(s, F_speed)) snprintf(b, sizeof b, "%d", (int)lroundf(fv(s, F_speed))); else snprintf(b, sizeof b, "--");
    d_value_set(&lv_spd, b);
    if (have_f(s, F_rpm)) snprintf(b, sizeof b, "%d", (int)lroundf(fv(s, F_rpm))); else snprintf(b, sizeof b, "--");
    d_value_set(&lv_rpm, b);
    /* measured row rate */
    if (lv_t0 && s->now_ms - lv_t0 >= 3000) {
        float hz = (float)(s->st->rows_written - lv_rows0) * 1000.0f / (float)(s->now_ms - lv_t0);
        if (s->obd_live && hz > 0) ui_set_fmt(lv_hz, "%.1f Hz", (double)hz); else ui_set_text(lv_hz, "no data");
        ui_set_color(lv_hz, s->obd_live && hz > 0 ? UI_GREEN : UI_MUTED);
        lv_t0 = 0;
    }
    if (!lv_t0) { lv_t0 = s->now_ms; lv_rows0 = s->st->rows_written; }
    const int F[4] = {F_throttle, F_load, F_coolant, F_fuel_level};
    static const char *const U[5] = {"%", "%", "C", "%", "V"};
    for (int k = 0; k < 5; k++) {
        bool ok = k < 4 ? have_f(s, F[k]) : s->obd_live && s->st->obd.adapter_v_valid;
        float v = k < 4 ? fv(s, F[k]) : s->st->obd.adapter_voltage_v;
        int barv = !ok ? 0 : k == 2 ? (int)fminf(fmaxf(v, 0) / 120.0f * 100, 100) : k == 4 ? (int)fminf(fmaxf((v - 10) / 5.0f * 100, 0), 100)
                       : (int)fminf(fmaxf(v, 0), 100);
        bar_set(lv_b[k], barv);
        if (ok) ui_set_fmt(lv_v[k], k == 4 ? "%.1f %s" : "%.0f %s", (double)v, U[k]); else ui_set_text(lv_v[k], "--");
    }
}

/* ======================= Fuel ======================= */
static lv_obj_t *f_tag, *f_src_note, *f_use, *f_drive_note, *f_cal_note, *f_lb, *f_fb, *f_lv, *f_fv, *f_verdict;
static d_val_t f_rate, f_drive, f_cal;

static void fuel_create(lv_obj_t *p)
{
    d_header(p, "Fuel");
    f_tag = ui_pill(s_hr, "--", UI_MUTED);
    lv_obj_t *c = d_card(p, DX, DY, DW, 112, UI_ORANGE, true);
    d_lab(c, "FUEL RATE NOW", 0, 0, UI_ORANGE);
    f_rate = d_value(c, UI_THEME.f_hero, "L/h", 0, 14);
    f_src_note = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 72);
    lv_obj_t *l = d_lab(c, "USE NOW", 0, 0, UI_MUTED);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, 0);
    f_use = ui_label(c, UI_THEME.f_large, UI_TEXT, "--");
    lv_obj_align(f_use, LV_ALIGN_TOP_RIGHT, 0, 18);
    l = ui_label(c, UI_THEME.f_label, UI_MUTED, "L/100 km");
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, 54);
    int hw = (DW - 8) / 2;
    c = d_card(p, DX, DY + 120, hw, 88, UI_BORDER, false);
    d_lab(c, "THIS DRIVE", 0, 0, UI_MUTED);
    f_drive = d_value(c, UI_THEME.f_large, "", 0, 14);
    f_drive_note = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 48);
    c = d_card(p, DX + hw + 8, DY + 120, hw, 88, UI_BORDER, false);
    d_lab(c, "FILL-UPS", 0, 0, UI_MUTED);
    f_cal = d_value(c, UI_THEME.f_large, "factor", 0, 14);
    f_cal_note = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 48);
    c = d_card(p, DX, DY + 216, DW, 126, UI_BORDER, false);
    d_lab(c, "LEARNER VS FACTORY MODEL, ERROR", 0, 0, UI_MUTED);
    d_txt(c, UI_THEME.f_label, UI_GREEN, "Learned", 0, 24);
    f_lb = d_bar(c, 80, 29, 230, 10, UI_GREEN, 1000);
    f_lv = ui_label(c, UI_THEME.f_label, UI_TEXT, "--");
    lv_obj_align(f_lv, LV_ALIGN_TOP_RIGHT, 0, 24);
    d_txt(c, UI_THEME.f_label, UI_MUTED, "Factory", 0, 50);
    f_fb = d_bar(c, 80, 55, 230, 10, UI_THEME.dim, 1000);
    f_fv = ui_label(c, UI_THEME.f_label, UI_TEXT, "--");
    lv_obj_align(f_fv, LV_ALIGN_TOP_RIGHT, 0, 50);
    f_verdict = d_txt(c, UI_THEME.f_label, UI_GREEN, "", 0, 80);
}

static void fuel_update(const ui_snap_t *s)
{
    const obd_values_t *o = &s->st->obd;
    const ml_stats_t *m = &s->ml;
    bool live = s->obd_live && o->fuel_lph_calc >= 0;
    char b[24];
    if (live) snprintf(b, sizeof b, "%.1f", (double)o->fuel_lph_calc); else snprintf(b, sizeof b, "--");
    d_value_set(&f_rate, b);
    const char *src = live ? fuel_src_name((fuel_src_t)o->fuel_src) : "";
    if (live) {
        char up[12]; int i = 0;
        for (; src[i] && i < 11; i++) up[i] = (char)toupper((unsigned char)src[i]);
        up[i] = 0;
        pill_set(f_tag, up, econ_src_is_estimate((fuel_src_t)o->fuel_src) ? UI_ORANGE : UI_GREEN);
        ui_set_fmt(f_src_note, "source: %s", src);
    } else { pill_set(f_tag, "NO SOURCE", UI_MUTED); ui_set_text(f_src_note, "no fuel figure: needs a drive"); }
    if (s->obd_live && o->econ_inst_kpl > 0.5f) ui_set_fmt(f_use, "%.1f", 100.0 / o->econ_inst_kpl); else ui_set_text(f_use, "--");
    const char *u = ui_econ(o->econ_trip_kpl, b, sizeof b);
    d_value_set(&f_drive, b);
    ui_set_text(f_drive.u, u);
    if (o->trip_km > 0.01) ui_set_fmt(f_drive_note, "%.2f L over %.1f km", (double)o->trip_fuel_l, o->trip_km); else ui_set_text(f_drive_note, "no distance yet");
    snprintf(b, sizeof b, "%.2f", (double)s->rs.cal);
    d_value_set(&f_cal, b);
    ui_set_fmt(f_cal_note, "from %d fill-up%s", s->rs.used, s->rs.used == 1 ? "" : "s");
    const rev_status_t *r = &s->rs;
    if (r->n_err[0] && r->n_err[1]) {
        float el = sqrtf(r->err[1] / r->n_err[1]), ef = sqrtf(r->err[0] / r->n_err[0]), mx = fmaxf(fmaxf(el, ef), 0.01f) * 1.4f;
        bar_set(f_lb, (int)(el / mx * 1000)); bar_set(f_fb, (int)(ef / mx * 1000));
        ui_set_fmt(f_lv, "%.2f L/h", (double)el); ui_set_fmt(f_fv, "%.2f L/h", (double)ef);
        int d = ef > 0 ? (int)lroundf(100 * (ef - el) / ef) : 0;
        if (d >= 0) ui_set_fmt(f_verdict, "%d %% lower error than factory on this car", d); else ui_set_fmt(f_verdict, "%d %% higher error than factory so far", -d);
        ui_set_color(f_verdict, d >= 0 ? UI_GREEN : UI_WARN);
    } else {
        bar_set(f_lb, 0); bar_set(f_fb, 0); ui_set_text(f_lv, "--"); ui_set_text(f_fv, "--");
        ui_set_text(f_verdict, !rev_in_use(s) ? "learning is off while E.V runs" : m->learned_ok ? "comparing: needs measured fuel labels"
                                                                                              : "learner waits for measured fuel labels");
        ui_set_color(f_verdict, UI_MUTED);
    }
}


/* ======================= Brakes and torque: 60 s line with a translucent area (a bar series under the line) ====== */
static lv_obj_t *br_ch[2], *br_area[2], *br_tag[2], *br_note[2], *br_peak[2];
static lv_chart_series_t *br_s[2], *br_sa[2];
static d_val_t br_v[2];
static float br_hist[2][60];
static int br_n;

static void br_card(lv_obj_t *p, int k, int y, const char *label, const char *tag, const char *unit, uint32_t color, int max)
{
    lv_obj_t *c = d_card(p, DX, y, DW, 167, color, true);
    d_lab(c, label, 0, 8, color);
    br_tag[k] = at(ui_pill(c, tag, UI_THEME.estimate), k ? 140 : 156, 4);
    br_v[k] = d_value(c, UI_THEME.f_big, unit, 290, 0);
    for (int j = 0; j < 2; j++) {                        /* j 0: the area (bars, translucent), j 1: the line on top */
        lv_obj_t *ch = lv_chart_create(c);
        lv_obj_set_pos(ch, 0, 42);
        lv_obj_set_size(ch, DW - 32, 78);
        lv_chart_set_type(ch, j ? LV_CHART_TYPE_LINE : LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(ch, 60);
        lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, max);
        lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
        lv_chart_set_div_line_count(ch, j ? 0 : 3, 0);
        lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ch, 0, 0);
        lv_obj_set_style_pad_all(ch, 0, 0);
        lv_obj_set_style_line_color(ch, lv_color_hex(UI_THEME.track), LV_PART_MAIN);
        lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
        lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
        lv_obj_set_style_pad_column(ch, 0, 0);
        lv_obj_set_style_pad_column(ch, 0, LV_PART_ITEMS);
        lv_obj_set_style_radius(ch, 0, LV_PART_ITEMS);
        if (!j) lv_obj_set_style_bg_opa(ch, LV_OPA_20, LV_PART_ITEMS);
        lv_obj_remove_flag(ch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ch, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_chart_series_t *se = lv_chart_add_series(ch, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_values(ch, se, LV_CHART_POINT_NONE);
        if (j) { br_ch[k] = ch; br_s[k] = se; } else { br_area[k] = ch; br_sa[k] = se; }
    }
    br_note[k] = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 126);
    br_peak[k] = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    lv_obj_align(br_peak[k], LV_ALIGN_TOP_RIGHT, 0, 126);
}

static void brakes_create(lv_obj_t *p)
{
    d_header(p, "Brakes and torque");
    d_right(p, "last 60 s", UI_MUTED);
    br_card(p, 0, DY, "BRAKE PRESSURE", "MODEL", "bar", UI_THEME.section[SEC_RED], 20);
    br_card(p, 1, DY + 175, "ENGINE TORQUE", "MODEL", "N m", UI_THEME.section[SEC_AMBER], 300);
    br_n = 0;
}

static void brakes_destroy(void) { br_s[0] = br_s[1] = br_sa[0] = br_sa[1] = NULL; }

static void brakes_update(const ui_snap_t *s)
{
    const ml_stats_t *m = &s->ml;
    const obd_values_t *o = &s->st->obd;
    bool have = m->last_class >= 0 && s->obd_live;
    bool tq = torque_physics(s);
    float v[2] = { have ? m->last_est[0] : NAN, tq ? o->torque_nm : (have ? m->last_est[1] : NAN) };
    pill_set(br_tag[1], TORQUE_SRC(s), UI_THEME.estimate);
    if (br_n == 60) { memmove(br_hist[0], br_hist[0] + 1, 59 * sizeof(float)); memmove(br_hist[1], br_hist[1] + 1, 59 * sizeof(float)); br_n = 59; }
    for (int k = 0; k < 2; k++) {
        br_hist[k][br_n] = v[k];
        char b[16];
        if (isfinite(v[k])) snprintf(b, sizeof b, k ? "%.0f" : "%.1f", (double)v[k]); else snprintf(b, sizeof b, "--");
        d_value_set(&br_v[k], b);
        int32_t pt = isfinite(v[k]) ? (int32_t)lroundf(fmaxf(v[k], 0)) : LV_CHART_POINT_NONE;
        lv_chart_set_next_value(br_ch[k], br_s[k], pt);
        lv_chart_set_next_value(br_area[k], br_sa[k], pt);
    }
    br_n++;
    float peak[2] = {-1, -1};
    for (int i = 0; i < br_n; i++) for (int k = 0; k < 2; k++) if (isfinite(br_hist[k][i]) && br_hist[k][i] > peak[k]) peak[k] = br_hist[k][i];
    if (peak[0] >= 0) ui_set_fmt(br_peak[0], "peak %.1f bar", (double)peak[0]); else ui_set_text(br_peak[0], "");
    if (peak[1] >= 0) ui_set_fmt(br_peak[1], "peak %.0f N m", (double)peak[1]); else ui_set_text(br_peak[1], "");
    int be = 0, hb = 0;
    mosaico_drive_live(&be, &hb);
    ui_set_fmt(br_note[0], "%d braking%s, %d hard this drive", be, be == 1 ? "" : "s", hb);
    if (tq) ui_set_text(br_note[1], "5 s mean, from fuel and rpm");
    else ui_set_fmt(br_note[1], "%s estimate (no physics torque yet)", model_tag(s));
}

/* ======================= Trip ======================= */
static lv_obj_t *t_log_dot, *t_log, *t_on, *t_tv[3], *t_tu[3], *t_sess, *t_rows;
static d_val_t t_dist;

static void trip_create(lv_obj_t *p)
{
    lv_obj_t *t = d_header(p, "Trip");
    d_after(t, ui_label(p, UI_THEME.f_label, UI_MUTED, "this drive so far"));
    t_log = d_right(p, "", UI_GREEN);
    t_log_dot = d_before(t_log, d_dot(p, UI_GREEN));
    lv_obj_t *c = d_card(p, DX, DY, DW, 96, UI_THEME.section[SEC_CYAN], true);
    d_lab(c, "DISTANCE", 0, 0, UI_THEME.section[SEC_CYAN]);
    t_dist = d_value(c, UI_THEME.f_hero, "km", 0, 14);
    lv_obj_t *l = d_lab(c, "ENGINE ON", 0, 0, UI_MUTED);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, 0);
    t_on = ui_label(c, UI_THEME.f_big, UI_TEXT, "--");
    lv_obj_align(t_on, LV_ALIGN_TOP_RIGHT, 0, 22);
    int w3 = (DW - 16) / 3;
    static const char *const L[3] = {"AVG SPEED", "FUEL USED", "ECONOMY"};
    for (int k = 0; k < 3; k++) {
        c = d_card(p, DX + k * (w3 + 8), DY + 104, w3, 88, UI_BORDER, false);
        d_lab(c, L[k], 0, 0, UI_MUTED);
        t_tv[k] = d_txt(c, UI_THEME.f_large, UI_TEXT, "--", 0, 16);
        t_tu[k] = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 50);
    }
    c = d_card(p, DX, DY + 200, DW, 142, UI_BORDER, false);
    d_lab(c, "WHEN THE DRIVE ENDS", 0, 0, UI_MUTED);
    l = d_txt(c, UI_THEME.f_body, UI_TEXT, 
                                           "Hold End drive for one second. The drive is saved, and Drive summary "
                                           
                                           "opens with the graphs.", 0, 22);
    lv_obj_set_width(l, DW - 28);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    t_sess = d_txt(c, UI_THEME.f_label, UI_MUTED, "", 0, 96);
    t_rows = ui_label(c, UI_THEME.f_label, UI_MUTED, "");
    lv_obj_align(t_rows, LV_ALIGN_TOP_RIGHT, 0, 96);
}

static void trip_update(const ui_snap_t *s)
{
    const obd_values_t *o = &s->st->obd;
    ui_set_text(t_log, s->drive_active ? "Logging" : "Not in a drive");
    ui_set_color(t_log, s->drive_active ? UI_GREEN : UI_MUTED);
    ui_set_bg(t_log_dot, s->drive_active ? UI_GREEN : UI_THEME.dim);
    char b[16];
    if (o->trip_km > 0.005) snprintf(b, sizeof b, "%.1f", o->trip_km); else snprintf(b, sizeof b, "--");
    d_value_set(&t_dist, b);
    bool rt = have_f(s, F_run_time);
    if (rt) ui_set_fmt(t_on, "%d:%02d", (int)(fv(s, F_run_time) / 3600), (int)fmodf(fv(s, F_run_time) / 60, 60)); else ui_set_text(t_on, "--");
    if (rt && fv(s, F_run_time) > 60 && o->trip_km > 0.05) ui_set_fmt(t_tv[0], "%.0f", o->trip_km / (fv(s, F_run_time) / 3600.0)); else ui_set_text(t_tv[0], "--");
    ui_set_text(t_tu[0], "km/h");
    if (o->trip_km > 0.005) ui_set_fmt(t_tv[1], "%.2f", (double)o->trip_fuel_l); else ui_set_text(t_tv[1], "--");
    ui_set_text(t_tu[1], "L");
    const char *u = ui_econ(o->econ_trip_kpl, b, sizeof b);
    ui_set_text(t_tv[2], b);
    ui_set_text(t_tu[2], u);
    if (s->st->sd_mounted) { ui_set_fmt(t_sess, "Session s%06lu", (unsigned long)s->st->session_id); ui_set_fmt(t_rows, "%lu rows", (unsigned long)s->st->rows_written); }
    else { ui_set_text(t_sess, "no log storage"); ui_set_text(t_rows, ""); }
}

/* ======================= Engine ======================= */
static lv_obj_t *e_mil, *e_card, *e_lab, *e_code, *e_desc, *e_cause, *e_chip[3], *e_sv[6], *e_su[6];

static int count_codes(const char *s)
{
    int n = 0;
    for (const char *p = s; p && *p;) { while (*p == ' ') p++; if (!*p) break; n++; while (*p && *p != ' ') p++; }
    return n;
}

static void engine_create(lv_obj_t *p)
{
    d_header(p, "Engine");
    e_mil = d_right(p, "", UI_MUTED);
    lv_obj_t *c = e_card = d_card(p, DX, DY, DW, 162, UI_THEME.dim, true);
    e_lab = d_lab(c, "TROUBLE CODES", 0, 0, UI_MUTED);
    e_code = d_txt(c, UI_THEME.f_big, UI_TEXT, "", 0, 16);
    e_desc = d_txt(c, UI_THEME.f_body, UI_TEXT, "", 0, 56);
    lv_obj_set_width(e_desc, DW - 32);
    lv_label_set_long_mode(e_desc, LV_LABEL_LONG_DOT);
    e_cause = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 78);
    lv_obj_set_width(e_cause, DW - 32);
    lv_label_set_long_mode(e_cause, LV_LABEL_LONG_WRAP);
    for (int k = 0; k < 3; k++) {
        e_chip[k] = at(ui_pill(c, "", UI_THEME.dim), k * 120, 114);
        ui_set_color(e_chip[k], UI_TEXT);
    }
    int w3 = (DW - 16) / 3;
    static const char *const L[6] = {"BATTERY", "INTAKE AIR", "AIRFLOW", "MANIFOLD", "SHORT TRIM", "LONG TRIM"};
    static const char *const U[6] = {"V", "C", "g/s", "kPa", "%", "%"};
    for (int k = 0; k < 6; k++) {
        c = d_card(p, DX + (k % 3) * (w3 + 8), DY + 170 + (k / 3) * 90, w3, 82, UI_BORDER, false);
        lv_obj_set_style_radius(c, 16, 0);
        d_lab(c, L[k], 0, 0, UI_MUTED);
        e_sv[k] = d_txt(c, UI_THEME.f_title, UI_TEXT, "--", 0, 24);
        e_su[k] = ui_label(c, UI_THEME.f_small, UI_MUTED, U[k]);
        lv_obj_align_to(e_su[k], e_sv[k], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -3);
    }
}

static void engine_update(const ui_snap_t *s)
{
    const obd_values_t *o = &s->st->obd;
    const fault_report_t *r = &s->st->faults;
    uint32_t mc = !o->status_valid ? UI_MUTED : o->mil_on ? UI_WARN : UI_GREEN;
    ui_set_text(e_mil, !o->status_valid ? "MIL not read" : o->mil_on ? LV_SYMBOL_WARNING " CHECK ENGINE ON" : "MIL off");
    ui_set_color(e_mil, mc);
    uint32_t bc = !o->status_valid ? UI_THEME.dim : mc;
    if (lv_color_to_u32(lv_obj_get_style_border_color(e_card, 0)) != lv_color_to_u32(lv_color_hex(bc))) lv_obj_set_style_border_color(e_card, lv_color_hex(bc), 0);
    ui_set_color(e_lab, o->status_valid ? mc : UI_MUTED);
    int ns = count_codes(o->dtcs), np = count_codes(o->pending_dtcs), nperm = count_codes(o->permanent_dtcs);
    if (r->n) {
        ui_set_text(e_code, r->faults[0].code);
        ui_set_color(e_code, UI_TEXT);
        ui_set_text(e_desc, r->faults[0].desc);
        ui_set_fmt(e_cause, "%s%s%s", r->insight, r->insight[0] && r->faults[0].cause ? " Likely: " : "", r->faults[0].cause ? r->faults[0].cause : "");
    } else if (o->status_valid || o->dtcs[0]) {
        ui_set_text(e_code, "No stored codes");
        ui_set_color(e_code, UI_GREEN);
        ui_set_text(e_desc, "");
        ui_set_text(e_cause, "Read from the car's engine computer.");
    } else {
        ui_set_text(e_code, "--");
        ui_set_color(e_code, UI_MUTED);
        ui_set_text(e_desc, "");
        ui_set_text(e_cause, "Codes are read once the car answers.");
    }
    ui_set_fmt(e_chip[0], "stored %d", ns);
    ui_set_fmt(e_chip[1], "pending %d", np);
    ui_set_fmt(e_chip[2], "permanent %d", nperm);
    const int F[5] = {F_iat, F_maf, F_map, F_stft1, F_ltft1};
    for (int k = 0; k < 6; k++) {
        bool ok; float v;
        if (k == 0) { ok = s->obd_live && o->adapter_v_valid; v = o->adapter_voltage_v; }
        else { ok = have_f(s, F[k - 1]); v = fv(s, F[k - 1]); }
        if (!ok) ui_set_text(e_sv[k], "--");
        else if (k == 0 || k == 2) ui_set_fmt(e_sv[k], "%.1f", (double)v);
        else if (k >= 4) ui_set_fmt(e_sv[k], "%+.0f", (double)v);
        else ui_set_fmt(e_sv[k], "%.0f", (double)v);
        ui_set_color(e_sv[k], k == 5 && ok && fabsf(v) > 10 ? UI_WARN : UI_TEXT);
        lv_obj_align_to(e_su[k], e_sv[k], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -3);
    }
}

/* ======================= Board: flex columns and rows; the deadline table is an lv grid sized to its texts ======= */
static lv_obj_t *o_dot, *o_jobs_ok, *o_touch, *o_ram_v, *o_ram_note, *o_psram, *o_bat_v, *o_bat_note, *o_cal, *o_store,
                *o_sbar, *o_fw, *o_chip;
#define O_ROWS 6
static lv_obj_t *o_rn[O_ROWS], *o_rr[O_ROWS], *o_rw[O_ROWS], *o_rb[O_ROWS], *o_rm[O_ROWS];

static lv_obj_t *fbox(lv_obj_t *p, lv_flex_flow_t f, lv_flex_align_t main, lv_flex_align_t cross, int gap)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_flow(b, f);
    lv_obj_set_flex_align(b, main, cross, cross);
    lv_obj_set_style_pad_gap(b, gap, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return b;
}
static lv_obj_t *frow(lv_obj_t *p) { lv_obj_t *r = fbox(p, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 6);
                                     lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT); return r; }
static lv_obj_t *fcard(lv_obj_t *p, int h)
{
    lv_obj_t *c = fbox(p, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 4);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_hor(c, 12, 0);
    lv_obj_set_style_pad_ver(c, 8, 0);
    lv_obj_set_height(c, h > 0 ? h : LV_SIZE_CONTENT);     /* 0 = as tall as its content */
    return c;
}

static void board_create(lv_obj_t *p)
{
    lv_obj_t *t = d_header(p, "Board");
    o_dot = d_after(t, d_dot(p, UI_GREEN));
    o_jobs_ok = d_after(t, ui_label(p, UI_THEME.f_label, UI_GREEN, ""));
    o_touch = d_right(p, "", UI_MUTED);
    lv_obj_t *col = fbox(p, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_pos(col, DX, DY);
    lv_obj_set_size(col, DW, UI_H - 16 - DY);
    lv_obj_t *top = fbox(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_size(top, LV_PCT(100), 88);
    lv_obj_t *c;
    c = fcard(top, 88);
    lv_obj_set_flex_grow(c, 1);
    d_lab(c, "RAM FREE", 0, 0, UI_MUTED);
    o_ram_v = ui_label(c, UI_THEME.f_value, UI_TEXT, "--");
    o_ram_note = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    o_psram = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    c = fcard(top, 88);
    lv_obj_set_flex_grow(c, 1);
    d_lab(c, "BATTERY", 0, 0, UI_MUTED);
    o_bat_v = ui_label(c, UI_THEME.f_value, UI_TEXT, "--");
    o_bat_note = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    o_cal = ui_label(c, UI_THEME.f_small, UI_WARN, "");
    c = fcard(col, 0);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_t *r = frow(c);
    d_lab(r, "LOG STORAGE", 0, 0, UI_MUTED);
    o_store = ui_label(r, UI_THEME.f_label, UI_TEXT, "--");
    o_sbar = lv_bar_create(c);
    lv_obj_set_size(o_sbar, LV_PCT(100), 8);
    lv_bar_set_range(o_sbar, 0, 1000);
    lv_obj_set_style_bg_color(o_sbar, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_bg_opa(o_sbar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o_sbar, lv_color_hex(UI_ORANGE), LV_PART_INDICATOR);
    lv_obj_remove_flag(o_sbar, LV_OBJ_FLAG_CLICKABLE);
    /* the deadline table: column widths from the widest text each one holds */
    c = fcard(col, 0);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_flex_grow(c, 1);
    r = frow(c);
    d_lab(r, "SOFT DEADLINES", 0, 0, UI_MUTED);
    ui_label(r, UI_THEME.f_small, UI_MUTED, "bar: worst run of 50 ms");
    lv_obj_t *g = lv_obj_create(c);
    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    static const int32_t cols[] = {LV_GRID_FR(3), LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_FR(2), LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    static int32_t rows[O_ROWS + 2];
    for (int i = 0; i < O_ROWS + 1; i++) rows[i] = LV_GRID_CONTENT;
    rows[O_ROWS + 1] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(g, cols, rows);
    lv_obj_set_style_pad_column(g, 12, 0);
    lv_obj_set_style_pad_row(g, 3, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    static const char *const H[5] = {"JOB", "RUNS", "WORST", "", "MISSED"};
    for (int k = 0; k < 5; k++) {
        lv_obj_t *l = ui_label(g, UI_THEME.f_small, UI_MUTED, H[k]);
        lv_obj_set_grid_cell(l, k == 0 || k == 3 ? LV_GRID_ALIGN_START : LV_GRID_ALIGN_END, k, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    }
    for (int i = 0; i < O_ROWS; i++) {
        o_rn[i] = ui_label(g, UI_THEME.f_small, UI_TEXT, "");
        lv_label_set_long_mode(o_rn[i], LV_LABEL_LONG_DOT);
        lv_obj_set_grid_cell(o_rn[i], LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, i + 1, 1);
        o_rr[i] = ui_label(g, UI_THEME.f_small, UI_MUTED, "");
        lv_obj_set_grid_cell(o_rr[i], LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, i + 1, 1);
        o_rw[i] = ui_label(g, UI_THEME.f_small, UI_TEXT, "");
        lv_obj_set_grid_cell(o_rw[i], LV_GRID_ALIGN_END, 2, 1, LV_GRID_ALIGN_CENTER, i + 1, 1);
        o_rb[i] = lv_bar_create(g);
        lv_obj_set_height(o_rb[i], 6);
        lv_bar_set_range(o_rb[i], 0, 100);
        lv_obj_set_style_bg_color(o_rb[i], lv_color_hex(UI_THEME.track), 0);
        lv_obj_set_style_bg_opa(o_rb[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(o_rb[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_grid_cell(o_rb[i], LV_GRID_ALIGN_STRETCH, 3, 1, LV_GRID_ALIGN_CENTER, i + 1, 1);
        o_rm[i] = ui_label(g, UI_THEME.f_small, UI_GREEN, "");
        lv_obj_set_grid_cell(o_rm[i], LV_GRID_ALIGN_END, 4, 1, LV_GRID_ALIGN_CENTER, i + 1, 1);
    }
    r = frow(col);
    o_fw = ui_label(r, UI_THEME.f_small, UI_MUTED, CARMIND_FW_STRING);
    o_chip = ui_label(r, UI_THEME.f_small, UI_MUTED, "");
}

static void board_update(const ui_snap_t *s)
{
    static sched_stats_t js[12];
    int n = sched_get_stats(js, 12);
    uint32_t miss = 0;
    for (int i = 0; i < n; i++) miss += js[i].misses;
    ui_set_bg(o_dot, miss ? UI_WARN : UI_GREEN);
    if (miss) ui_set_fmt(o_jobs_ok, "%lu missed", (unsigned long)miss); else ui_set_text(o_jobs_ok, "All jobs on time");
    ui_set_color(o_jobs_ok, miss ? UI_WARN : UI_GREEN);
    ui_set_fmt(o_touch, "touch errors %lu", (unsigned long)mosaico_touch_errors());
    ui_set_fmt(o_ram_v, "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ui_set_fmt(o_ram_note, "internal, lowest %u", (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ui_set_fmt(o_psram, "PSRAM %.1f MB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
    if (s->gauge.ok) {
        ui_set_fmt(o_bat_v, "%.2f V", s->gauge.mv / 1000.0);
        ui_set_fmt(o_bat_note, "%d mA, %s", s->gauge.ma, s->gauge.ma > -5 ? "on USB" : "on battery");
        if (mosaico_gauge_uncalibrated(&s->gauge)) { ui_set_text(o_cal, "% not calibrated"); ui_set_color(o_cal, UI_WARN); }
        else { ui_set_fmt(o_cal, "%d %%", s->gauge.soc); ui_set_color(o_cal, UI_MUTED); }
    } else { ui_set_text(o_bat_v, "--"); ui_set_text(o_bat_note, "gauge not read"); ui_set_text(o_cal, ""); }
    if (s->fat.ok && s->fat.total_kb) {
#if CONFIG_OCM_NAND_LOGS
        if (s->fat.nand) ui_set_fmt(o_store, "NAND %.1f of %.1f MB free, flash %lu KB", s->fat.free_kb / 1024.0,
                                    s->fat.total_kb / 1024.0, (unsigned long)s->fat.flash_free_kb);
        else
#endif
        ui_set_fmt(o_store, "%lu of %lu KB free", (unsigned long)s->fat.free_kb, (unsigned long)s->fat.total_kb);
        bar_set(o_sbar, (int)((uint64_t)(s->fat.total_kb - s->fat.free_kb) * 1000 / s->fat.total_kb));
        lv_obj_set_style_bg_color(o_sbar, lv_color_hex(s->fat.free_kb * 10 < s->fat.total_kb ? UI_RED : UI_ORANGE), LV_PART_INDICATOR);
    } else ui_set_text(o_store, s->st->sd_mounted ? "measuring" : "not mounted");
    for (int i = 0; i < O_ROWS; i++) {
        bool on = i < n;
        lv_obj_t *cells[5] = {o_rn[i], o_rr[i], o_rw[i], o_rb[i], o_rm[i]};
        for (int k = 0; k < 5; k++) { if (on) lv_obj_remove_flag(cells[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(cells[k], LV_OBJ_FLAG_HIDDEN); }
        if (!on) continue;
        ui_set_fmt(o_rn[i], "%.16s", js[i].name);
        ui_set_fmt(o_rr[i], "%lu", (unsigned long)js[i].runs);
        ui_set_fmt(o_rw[i], "%.1f ms", js[i].max_us / 1000.0);
        int pv = (int)fminf(fmaxf(js[i].max_us / 500.0f, 2), 100);
        bar_set(o_rb[i], pv);
        lv_obj_set_style_bg_color(o_rb[i], lv_color_hex(pv > 80 ? UI_RED : pv > 50 ? UI_WARN : UI_ORANGE), LV_PART_INDICATOR);
        ui_set_fmt(o_rm[i], "%lu", (unsigned long)js[i].misses);
        ui_set_color(o_rm[i], js[i].misses ? UI_RED : UI_GREEN);
    }
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    ui_set_fmt(o_chip, "ESP32-S31 rev %d.%d  IDF %.8s", ci.revision / 100, ci.revision % 100, esp_get_idf_version() + (esp_get_idf_version()[0] == 'v'));
}

/* ======================= Drive (H_Drive): dark = pure black and near-black panels, light = white panels on the light
   ground (theme panel tokens). Flex rows and columns throughout ===== */
#define RPM_BLOCKS 20
#define TANK_BLOCKS 10
#define DCLS (UI_THEME.dcls)                               /* stopped, braking, accelerating, cruising */
static lv_obj_t *d_ctl, *d_spd, *d_rpm, *d_rpm_blk[RPM_BLOCKS], *d_ai, *d_face[4], *d_cls, *d_sure, *d_pb[4], *d_mark,
                *d_ev[3], *d_tag[3], *d_tank_blk[TANK_BLOCKS], *d_tank_pct, *d_tank_km;
static int d_rpm_lit = -1, d_tank_lit = -1;

static lv_obj_t *dk_card(lv_obj_t *p, int radius)
{
    lv_obj_t *c = fbox(p, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, 4);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_THEME.panel), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(UI_THEME.panel_edge), 0);
    lv_obj_set_style_radius(c, radius, 0);
    lv_obj_set_style_pad_hor(c, 14, 0);
    lv_obj_set_style_pad_ver(c, 10, 0);
    return c;
}
static lv_obj_t *dk_lab(lv_obj_t *p, const char *t)
{
    lv_obj_t *l = ui_label(p, UI_THEME.f_small, UI_THEME.label2, t);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}
static lv_obj_t *dk_tag(lv_obj_t *p, const char *t, uint32_t c)
{
    lv_obj_t *l = ui_label(p, UI_THEME.f_small, c, t);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_color(l, lv_color_hex(c), 0);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(l, 5, 0);
    return l;
}
static lv_obj_t *dk_blocks(lv_obj_t *p, lv_obj_t **blk, int n, int h)
{
    lv_obj_t *r = fbox(p, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 2);
    lv_obj_set_size(r, LV_PCT(100), h);
    for (int i = 0; i < n; i++) { blk[i] = ui_rect(r, 0, 0, 1, h, UI_THEME.panel_edge, 2); lv_obj_set_flex_grow(blk[i], 1); }
    return r;
}

static void drive_create(lv_obj_t *p)
{
    ui_back(p, 22, 51, 98, UI_THEME.chip, UI_THEME.section[SEC_SKY], NULL);
    lv_obj_t *col = fbox(p, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_pos(col, 22, 54);
    lv_obj_set_size(col, UI_W - 44, UI_H - 16 - 54);
    /* header: back, Drive, End drive pill. The row is y 54-91 (no counter pill on Drive: the status bar ends at y 44);
       the 44 px chevron is drawn at y 51-94 and its 104 x 98 touch area ends above the speed row (y 100) */
    lv_obj_t *hr = fbox(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 10);
    lv_obj_set_size(hr, LV_PCT(100), 38);
    lv_obj_t *b = fbox(hr, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 0);   /* the chevron's place */
    lv_obj_set_size(b, UI_BACK_D, 1);
    ui_label(hr, UI_THEME.f_value, UI_TEXT, "Drive");
    lv_obj_t *sp = fbox(hr, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_flex_grow(sp, 1);
    d_ctl = ui_drive_pill(hr, 0, 0, 168, 38);
    /* speed and rpm */
    lv_obj_t *r = fbox(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_size(r, LV_PCT(100), 112);
    lv_obj_t *c = dk_card(r, 20);
    lv_obj_set_height(c, 112);
    lv_obj_set_flex_grow(c, 5);
    dk_lab(c, "SPEED");
    lv_obj_t *vr = fbox(c, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, 8);
    lv_obj_set_size(vr, LV_PCT(100), LV_SIZE_CONTENT);
    d_spd = ui_label(vr, UI_THEME.f_hero, UI_THEME.section[SEC_SKY], "--");
    lv_obj_t *u = ui_label(vr, UI_THEME.f_label, UI_THEME.label2, "km/h");
    lv_obj_set_style_pad_bottom(u, 8, 0);
    c = dk_card(r, 20);
    lv_obj_set_height(c, 112);
    lv_obj_set_flex_grow(c, 4);
    dk_lab(c, "RPM");
    d_rpm = ui_label(c, UI_THEME.f_big, UI_TEXT, "--");
    dk_blocks(c, d_rpm_blk, RPM_BLOCKS, 14);
    /* AI SEES: dark card with a 4 px class-colour left edge, pixel face, class, S/B/A/C bars, model mark */
    d_ai = fbox(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 12);
    lv_obj_set_size(d_ai, LV_PCT(100), 82);
    lv_obj_set_style_bg_color(d_ai, lv_color_hex(UI_THEME.panel), 0);
    lv_obj_set_style_bg_opa(d_ai, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d_ai, 4, 0);
    lv_obj_set_style_border_side(d_ai, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(d_ai, lv_color_hex(DCLS[3]), 0);
    lv_obj_set_style_radius(d_ai, 20, 0);
    lv_obj_set_style_pad_hor(d_ai, 12, 0);
    lv_obj_t *fb = ui_rect(d_ai, 0, 0, 56, 56, UI_THEME.inset, 14);
    d_face[0] = ui_rect(fb, 8, 8, 40, 40, 0, 0);                    /* the head outline, 4 px, class colour */
    lv_obj_set_style_bg_opa(d_face[0], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d_face[0], 4, 0);
    d_face[1] = ui_rect(fb, 20, 22, 5, 5, DCLS[3], 0);
    d_face[2] = ui_rect(fb, 32, 22, 5, 5, DCLS[3], 0);
    d_face[3] = ui_rect(fb, 20, 34, 17, 4, DCLS[3], 0);
    lv_obj_t *tc = fbox(d_ai, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, 2);
    lv_obj_set_height(tc, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tc, 1);
    dk_lab(tc, "AI SEES");
    lv_obj_t *cr = fbox(tc, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, 8);
    lv_obj_set_size(cr, LV_PCT(100), LV_SIZE_CONTENT);
    d_cls = ui_label(cr, UI_THEME.f_title, DCLS[3], "--");
    d_sure = ui_label(cr, UI_THEME.f_label, UI_MUTED, "");
    lv_obj_set_style_pad_bottom(d_sure, 3, 0);
    lv_obj_t *pr = fbox(d_ai, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, 4);
    lv_obj_set_size(pr, LV_SIZE_CONTENT, 56);
    static const char *const K[4] = {"S", "B", "A", "C"};
    for (int k = 0; k < 4; k++) {
        lv_obj_t *pc = fbox(pr, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, 2);
        lv_obj_set_size(pc, 14, 56);
        d_pb[k] = lv_bar_create(pc);
        lv_obj_set_size(d_pb[k], 12, 38);
        lv_bar_set_range(d_pb[k], 0, 100);
        lv_obj_set_style_bg_color(d_pb[k], lv_color_hex(UI_THEME.inset_hi), 0);
        lv_obj_set_style_bg_opa(d_pb[k], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(d_pb[k], 3, 0);
        lv_obj_set_style_radius(d_pb[k], 0, LV_PART_INDICATOR);
        lv_obj_remove_flag(d_pb[k], LV_OBJ_FLAG_CLICKABLE);
        ui_label(pc, UI_THEME.f_small, UI_THEME.label2, K[k]);
    }
    d_mark = lv_image_create(d_ai);
    lv_image_set_src(d_mark, ml_model_active() == ML_MODEL_REV ? &ocm_mark_rev_34 : &ocm_mark_ev_34);
    /* three estimate tiles: label + outlined tag, value + unit */
    r = fbox(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_size(r, LV_PCT(100), 78);
    static const char *const EL[3] = {"BRAKE", "TORQUE", "FUEL"}, *const ET[3] = {"MODEL", "MODEL", "LEARNED"};
    /* fixed widths, 420 px with the two 8 px gaps. With 1 px letter spacing and 4 px pill padding the label row needs
       128 px for "TORQUE" + "PHYSICS", 110 for "BRAKE" + "MODEL" and 112 for "FUEL" + "LEARNED" (Montserrat 12 advance
       widths); each tile leaves width - 2 x 8 padding - 2 x 1 border for it */
    static const int EW[3] = {134, 152, 134};
    const uint32_t EC[3] = {DCLS[1], UI_THEME.section[SEC_AMBER], UI_THEME.section[SEC_GREEN]},
                   TC[3] = {UI_THEME.section[SEC_AMBER], UI_THEME.section[SEC_AMBER], UI_THEME.section[SEC_GREEN]};
    for (int k = 0; k < 3; k++) {
        c = dk_card(r, 18);
        lv_obj_set_size(c, EW[k], 78);
        lv_obj_set_style_pad_hor(c, 8, 0);
        lv_obj_t *hrow = frow(c);
        lv_obj_set_style_text_letter_space(dk_lab(hrow, EL[k]), 1, 0);
        d_tag[k] = dk_tag(hrow, ET[k], TC[k]);
        lv_obj_set_style_pad_hor(d_tag[k], 4, 0);
        d_ev[k] = ui_label(c, UI_THEME.f_value, EC[k], "--");
    }
    /* tank */
    c = fbox(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 10);
    lv_obj_set_size(c, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_THEME.panel), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(UI_THEME.panel_edge), 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_hor(c, 14, 0);
    dk_lab(c, "TANK");
    lv_obj_t *tb = dk_blocks(c, d_tank_blk, TANK_BLOCKS, 12);
    lv_obj_set_width(tb, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tb, 1);
    d_tank_pct = ui_label(c, UI_THEME.f_label, UI_TEXT, "--");
    d_tank_km = ui_label(c, UI_THEME.f_small, UI_THEME.label2, "");
    d_rpm_lit = d_tank_lit = -1;
}

static void drive_destroy(void) { d_ctl = NULL; }

static void drive_update(const ui_snap_t *s)
{
    const obd_values_t *o = &s->st->obd;
    const ml_stats_t *m = &s->ml;
    set_int_or_dash(d_spd, have_f(s, F_speed), fv(s, F_speed));
    bool rpm_ok = have_f(s, F_rpm);
    set_int_or_dash(d_rpm, rpm_ok, fv(s, F_rpm));
    int lit = rpm_ok ? (int)lroundf(fminf(fv(s, F_rpm), 7000) / 7000.0f * RPM_BLOCKS) : 0;
    if (lit != d_rpm_lit) {
        d_rpm_lit = lit;
        for (int i = 0; i < RPM_BLOCKS; i++) ui_set_bg(d_rpm_blk[i], i >= lit ? UI_THEME.panel_edge : i >= RPM_BLOCKS - 4 ? UI_THEME.section[SEC_RED]
                                                                                           : UI_THEME.section[SEC_SKY]);
    }
    bool have = m->last_class >= 0;
    uint32_t cc = have ? DCLS[m->last_class] : UI_THEME.off;
    if (lv_color_to_u32(lv_obj_get_style_border_color(d_ai, 0)) != lv_color_to_u32(lv_color_hex(cc))) lv_obj_set_style_border_color(d_ai, lv_color_hex(cc), 0);
    if (lv_color_to_u32(lv_obj_get_style_border_color(d_face[0], 0)) != lv_color_to_u32(lv_color_hex(cc))) lv_obj_set_style_border_color(d_face[0], lv_color_hex(cc), 0);
    for (int k = 1; k < 4; k++) ui_set_bg(d_face[k], cc);
    ui_set_text(d_cls, have ? ui_class_name(m->last_class) : "--");
    ui_set_color(d_cls, cc);
    if (have) ui_set_fmt(d_sure, "%d %% sure", pct(m->last_prob)); else ui_set_text(d_sure, m->ready ? "waits for a drive" : "model not loaded");
    for (int k = 0; k < 4; k++) {
        bar_set(d_pb[k], s->nprobs > k ? pct(s->probs[k]) : 0);
        lv_obj_set_style_bg_color(d_pb[k], lv_color_hex(have && k == m->last_class ? DCLS[k] : UI_THEME.off), LV_PART_INDICATOR);
    }
    mark_set(d_mark, model_mark_34(s));
    if (have) ui_set_fmt(d_ev[0], "%.1f bar", (double)m->last_est[0]); else ui_set_text(d_ev[0], "--");
    ui_set_text(d_tag[1], TORQUE_SRC(s));
    if (torque_physics(s)) ui_set_fmt(d_ev[1], "%.0f N m", (double)o->torque_nm);
    else if (have) ui_set_fmt(d_ev[1], "%.0f N m", (double)m->last_est[1]);
    else ui_set_text(d_ev[1], "--");
    if (m->learned_ok && rev_in_use(s)) { ui_set_fmt(d_ev[2], "%.1f L/h", (double)m->learned_lph); ui_set_text(d_tag[2], "LEARNED"); }
    else if (s->obd_live && o->fuel_lph_calc >= 0) {
        ui_set_fmt(d_ev[2], "%.1f L/h", (double)o->fuel_lph_calc);
        char up[12]; const char *src = fuel_src_name((fuel_src_t)o->fuel_src); int i = 0;
        for (; src[i] && i < 11; i++) up[i] = (char)toupper((unsigned char)src[i]);
        up[i] = 0;
        ui_set_text(d_tag[2], up);
    } else { ui_set_text(d_ev[2], "--"); ui_set_text(d_tag[2], rev_in_use(s) ? "LEARNED" : "MODEL"); }
    const rev_tank_out_t *t = &s->rs.tank;
    float level = t->ready ? t->level : have_f(s, F_fuel_level) ? fv(s, F_fuel_level) : -1;
    int tl = level >= 0 ? (int)lroundf(fminf(level, 100) / 10.0f) : 0;
    if (tl != d_tank_lit) { d_tank_lit = tl; for (int i = 0; i < TANK_BLOCKS; i++) ui_set_bg(d_tank_blk[i], i < tl ? UI_THEME.section[SEC_GREEN] : UI_THEME.panel_edge); }
    if (level >= 0) ui_set_fmt(d_tank_pct, "%.0f %%", (double)level); else ui_set_text(d_tank_pct, "--");
    if (o->tank_range_km >= 0) ui_set_fmt(d_tank_km, "about %.0f km", (double)o->tank_range_km); else ui_set_text(d_tank_km, "");
    ui_drive_ctl_refresh(d_ctl, s);
}

/* ======================= Faults (H_Faults): the fault engine's ranked report, a vertically scrolling list of cards ===== */
static lv_obj_t *q_mil, *q_count, *q_insight, *q_chip[3], *q_list;
static uint32_t q_rev = UINT32_MAX;
static int64_t q_read_ms;

static uint32_t sev_color(uint8_t sev)
{
    return sev >= SEV_CRITICAL ? UI_THEME.section[SEC_RED] : sev >= SEV_HIGH ? UI_THEME.section[SEC_ORANGE]
         : sev >= SEV_MEDIUM ? UI_WARN : UI_THEME.section[SEC_SKY];
}

static void faults_create(lv_obj_t *p)
{
    lv_obj_t *t = d_header(p, "Faults");
    q_mil = d_after(t, dk_tag(p, "MIL ON", UI_THEME.section[SEC_RED]));
    q_count = d_right(p, "", UI_MUTED);
    lv_obj_t *col = fbox(p, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_pos(col, DX, DY);
    lv_obj_set_size(col, DW, UI_H - 16 - DY);
    lv_obj_t *c = fcard(col, 0);
    lv_obj_set_width(c, LV_PCT(100));
    q_insight = ui_label(c, UI_THEME.f_label, UI_TEXT, "");
    lv_obj_set_width(q_insight, LV_PCT(100));
    lv_label_set_long_mode(q_insight, LV_LABEL_LONG_WRAP);
    lv_obj_t *r = fbox(c, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 8);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    for (int k = 0; k < 3; k++) q_chip[k] = dk_tag(r, "", UI_MUTED);
    /* the list: scrolls vertically with momentum, scrollbar on the right */
    q_list = lv_obj_create(col);
    lv_obj_remove_style_all(q_list);
    lv_obj_set_width(q_list, LV_PCT(100));
    lv_obj_set_flex_grow(q_list, 1);
    lv_obj_set_flex_flow(q_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(q_list, 8, 0);
    lv_obj_set_style_pad_right(q_list, 8, 0);
    lv_obj_set_scroll_dir(q_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(q_list, LV_SCROLLBAR_MODE_ON);
    lv_obj_add_flag(q_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_color(q_list, lv_color_hex(UI_MUTED), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(q_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(q_list, 4, LV_PART_SCROLLBAR);
    q_rev = UINT32_MAX;
}

static void faults_destroy(void) { q_list = NULL; }

static void faults_fill(const ui_snap_t *s)
{
    const fault_report_t *r = &s->st->faults;
    lv_obj_clean(q_list);
    if (!r->n) {
        lv_obj_t *c = fcard(q_list, 0);
        lv_obj_set_width(c, LV_PCT(100));
        ui_label(c, UI_THEME.f_value, UI_GREEN, r->mil_on ? "MIL on, no codes read yet" : "No codes, MIL off");
        lv_obj_set_width(ui_label(c, UI_THEME.f_label, UI_MUTED, "Stored, pending and permanent codes are read from the engine computer."),
                         LV_PCT(100));
        return;
    }
    for (int i = 0; i < r->n; i++) {
        const fault_t *f = &r->faults[i];
        lv_obj_t *c = fcard(q_list, 0);
        lv_obj_set_width(c, LV_PCT(100));
        lv_obj_set_style_border_width(c, 4, 0);
        lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(sev_color(f->severity)), 0);
        lv_obj_set_style_radius(c, 16, 0);
        lv_obj_t *hr = fbox(c, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 8);
        lv_obj_set_size(hr, LV_PCT(100), LV_SIZE_CONTENT);
        ui_label(hr, UI_THEME.f_title, UI_TEXT, f->code);
        if (f->is_new) dk_tag(hr, "NEW", UI_THEME.section[SEC_GREEN]);
        lv_obj_t *sp = fbox(hr, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_size(sp, 1, 1);
        lv_obj_set_flex_grow(sp, 1);
        ui_label(hr, UI_THEME.f_small, sev_color(f->severity), fault_severity_name(f->severity));
        lv_obj_t *d = ui_label(c, UI_THEME.f_label, UI_TEXT, f->desc);
        lv_obj_set_width(d, LV_PCT(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        char cause[96] = "";
        if (f->group >= 0 && f->group < r->ngroups) {
            const fault_group_t *g = &r->groups[f->group];
            if (g->n && g->members[0] != i) snprintf(cause, sizeof cause, "Same cause as %s", r->faults[g->members[0]].code);
            else snprintf(cause, sizeof cause, "%s: %s", g->title, g->cause);
        } else if (f->cause) snprintf(cause, sizeof cause, "%s", f->cause);
        lv_obj_t *br = fbox(c, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 8);
        lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_t *cl = ui_label(br, UI_THEME.f_small, UI_MUTED, cause);
        lv_label_set_long_mode(cl, LV_LABEL_LONG_WRAP);
        lv_obj_set_flex_grow(cl, 1);
        dk_tag(br, f->sources & DTC_PERMANENT ? "PERMANENT" : f->sources & DTC_STORED ? "STORED" : "PENDING", UI_MUTED);
    }
    lv_obj_t *end = ui_label(q_list, UI_THEME.f_small, UI_MUTED, "");
    ui_set_fmt(end, "End of list. Read %lld min ago.", (long long)((s->now_ms - q_read_ms) / 60000));
}

/* the card list is rebuilt only when the codes were read again (fault_rev), and after the timed ui job (lv_async) */
static const app_status_t *q_st;
static bool q_due;
static void faults_async(void *arg)
{
    (void)arg;
    q_due = false;
    if (!q_list || !q_st) return;
    int32_t y = lv_obj_get_scroll_y(q_list);
    ui_snap_t s = { .st = (app_status_t *)q_st, .now_ms = esp_timer_get_time() / 1000 };
    faults_fill(&s);
    lv_obj_update_layout(q_list);
    lv_obj_scroll_to_y(q_list, y, LV_ANIM_OFF);          /* the list keeps its place */
}

static void faults_update(const ui_snap_t *s)
{
    const fault_report_t *r = &s->st->faults;
    const obd_values_t *o = &s->st->obd;
    if (r->mil_on || o->mil_on) lv_obj_remove_flag(q_mil, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(q_mil, LV_OBJ_FLAG_HIDDEN);
    ui_set_fmt(q_count, "%d fault%s", r->n, r->n == 1 ? "" : "s");
    ui_set_text(q_insight, r->insight[0] ? r->insight : "No report yet: codes are read once the car answers.");
    int ns = 0, np = 0, nperm = 0;
    for (int i = 0; i < r->n; i++) { ns += !!(r->faults[i].sources & DTC_STORED); np += !!(r->faults[i].sources & DTC_PENDING);
                                     nperm += !!(r->faults[i].sources & DTC_PERMANENT); }
    ui_set_fmt(q_chip[0], "STORED %d", ns);
    ui_set_fmt(q_chip[1], "PENDING %d", np);
    ui_set_fmt(q_chip[2], "PERMANENT %d", nperm);
    q_st = s->st;
    if (s->st->fault_rev != q_rev) {
        q_rev = s->st->fault_rev; q_read_ms = s->now_ms;
        if (!q_due) { q_due = true; lv_async_call(faults_async, NULL); }
    }
}

/* ======================= Drive summary (D_Summary): 4 KPI tiles, speed + fuel lines, the AI timeline every 10 s, share
   chips; swipe up for the history page. Built when a drive ends (and on open), never per frame. ================ */
static lv_obj_t *sm_tv, *sm_p1, *sm_p2, *sm_hdr;
static uint32_t sm_gen = UINT32_MAX, sm_hgen = UINT32_MAX;
EXT_RAM_BSS_ATTR static md_summary_t sm;
EXT_RAM_BSS_ATTR static md_history_t sm_h;

static lv_obj_t *sm_chart(lv_obj_t *p, int x, int y, int w, int h, lv_chart_type_t type, int n, int max)
{
    lv_obj_t *ch = lv_chart_create(p);
    lv_obj_set_pos(ch, x, y);
    lv_obj_set_size(ch, w, h);
    lv_chart_set_type(ch, type);
    lv_chart_set_point_count(ch, n > 1 ? n : 2);
    lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, max > 0 ? max : 1);
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_line_color(ch, lv_color_hex(UI_THEME.track), LV_PART_MAIN);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    lv_obj_remove_flag(ch, LV_OBJ_FLAG_CLICKABLE);
    return ch;
}

/* ---- RECENT EVENTS (T1, owner: "see opencarmind how it shows logs like in drive summary"): the shared events.c ring
   as the S3 shows it (ui_events_logic.h). A card on the summary page (the newest lines that fit, the pattern footer);
   a tap opens the full list (filter chips, 64 px rows, newest first), a tap on a row its detail. Labels are made once
   and changed in place when the rows' fingerprint changes (no per-second rebuild, the list keeps its scroll). ---- */
static lv_color_t st_pressed(void);
static lv_obj_t *sm_parent, *ev_card, *ev_foot, *ev_line[EVENT_MAX];
static int ev_card_n;
static uint32_t ev_card_sig;
static lv_obj_t *ev_page, *ev_count, *ev_note, *ev_list, *ev_empty, *ev_chip[UEV_FILTERS], *ev_row[EVENT_MAX], *ev_row_when[EVENT_MAX],
                *ev_row_text[EVENT_MAX], *ev_det;
static int ev_filter, ev_row_ring[EVENT_MAX], ev_rows_n, ev_top_ring = -1;
static uint32_t ev_page_sig;
static const app_status_t *ev_st;                         /* the shell's status copy (it outlives the app) */
EXT_RAM_BSS_ATTR static uev_row_t ev_rows[EVENT_MAX];
EXT_RAM_BSS_ATTR static sys_event_t ev_ring[EVENT_MAX];   /* the ring as last shown: what a tapped row opens */
#define EV_ROW_H 64
#define EV_GAP   6

static uint32_t ev_color(uev_tone_t t)
{
    return t == UEV_GOOD ? UI_GREEN : t == UEV_WARN ? UI_WARN : t == UEV_ERROR ? UI_RED : t == UEV_MUTED ? UI_MUTED : UI_TEXT;
}

static void ev_ctx(uev_ctx_t *c)
{
    c->power = power_state(sdlog_ok(), sdlog_saved(), sdlog_ms_since_write(), sys_mode_get() == MODE_DRIVE, uploader_busy());
    c->mark_ok = LV_SYMBOL_OK;
    c->mark_warn = LV_SYMBOL_WARNING;
    c->win_ok = sm.ok && !sm.past && sm.start_up_ms > 0;  /* a drive of this boot: its window holds ring events */
    c->win_start_ms = sm.start_up_ms;
    c->win_dur_s = sm.dur_s;
}

static void ev_open(lv_event_t *e);
static void ev_card_build(lv_obj_t *pg, int y, int h)
{
    lv_obj_t *c = ev_card = d_card(pg, DX, y, DW, h, UI_BORDER, false);
    lv_obj_set_style_pad_ver(c, 8, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);   /* the whole card (>= 64 px) opens the list */
    lv_obj_set_style_bg_color(c, st_pressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, ev_open, LV_EVENT_CLICKED, NULL);
    d_lab(c, "RECENT EVENTS", 0, 0, UI_MUTED);
    ev_foot = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    lv_obj_align(ev_foot, LV_ALIGN_TOP_RIGHT, 0, 0);
    int lh = lv_font_get_line_height(UI_THEME.f_label) + 1;
    ev_card_n = (h - 16 - 17 + 1) / lh;                   /* lines from y 17 inside the padding (66 px card: 2) */
    if (ev_card_n > EVENT_MAX) ev_card_n = EVENT_MAX;
    if (ev_card_n < 1) ev_card_n = 1;
    memset(ev_line, 0, sizeof ev_line);
    for (int i = 0; i < ev_card_n; i++) {
        ev_line[i] = d_txt(c, UI_THEME.f_label, UI_TEXT, "", 0, 17 + i * lh);
        lv_obj_set_width(ev_line[i], DW - 28);
        lv_label_set_long_mode(ev_line[i], LV_LABEL_LONG_DOT);   /* one line each, cut with dots; the list shows it all */
    }
    ev_card_sig = 0;
}

static void ev_card_refresh(const ui_snap_t *s)
{
    if (!ev_card) return;
    const app_status_t *st = s->st;
    uev_ctx_t c;
    ev_ctx(&c);
    uint32_t sig = uev_sig(st->events, st->event_head, st->event_count, c.power, st->pattern_windows);
    if (sig == ev_card_sig) return;
    ev_card_sig = sig;
    int n = uev_rows(st->events, st->event_head, st->event_count, UEV_ALL, &c, ev_rows, ev_card_n);
    for (int i = 0; i < ev_card_n; i++) {
        if (i < n) { ui_set_text(ev_line[i], ev_rows[i].line); ui_set_color(ev_line[i], ev_color(ev_rows[i].tone)); }
        else { ui_set_text(ev_line[i], i ? "" : "no events yet"); ui_set_color(ev_line[i], UI_MUTED); }
    }
    char f[64];
    uev_pattern(f, sizeof f, st->pattern_windows, st->pattern_id, st->pattern_novelty, st->pattern_used);
    ui_set_fmt(ev_foot, "%s  " LV_SYMBOL_RIGHT, f);
}

/* the list's and the detail's header: a Back (ui_back, the same place and touch area as the app pages' Back) that closes
   the list or the detail, not the app, then the title */
static lv_obj_t *ev_head(lv_obj_t *p, const char *title, lv_event_cb_t back)
{
    ui_back(p, DX, DHY - 7, DY, UI_THEME.chip, UI_ORANGE, back);
    lv_obj_t *t = ui_label(p, UI_THEME.f_value, UI_TEXT, title);
    lv_obj_set_pos(t, DX + UI_BACK_D + 10, DHY - 7 + (UI_BACK_D - lv_font_get_line_height(UI_THEME.f_value)) / 2);
    return t;
}

static lv_obj_t *ev_screen(lv_obj_t *p)                    /* a full-screen panel in the palette over the app */
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);             /* swallows taps meant for what is under it */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void ev_close_list(void *arg) { (void)arg; if (ev_page) { lv_obj_delete(ev_page); ev_page = ev_det = NULL; } }
static void ev_close_detail(void *arg) { (void)arg; if (ev_det) { lv_obj_delete(ev_det); ev_det = NULL; } }
static void ev_list_back_cb(lv_event_t *e) { (void)e; lv_async_call(ev_close_list, NULL); }
static void ev_det_back_cb(lv_event_t *e) { (void)e; lv_async_call(ev_close_detail, NULL); }

static void ev_chip_paint(void)
{
    for (int i = 0; i < UEV_FILTERS; i++) {
        bool on = i == ev_filter;
        lv_obj_set_style_bg_color(ev_chip[i], lv_color_hex(on ? UI_ORANGE : UI_THEME.track), 0);
        lv_obj_set_style_bg_color(ev_chip[i], on ? lv_color_hex(UI_ORANGE) : st_pressed(), LV_STATE_PRESSED);
        ui_set_color(lv_obj_get_child(ev_chip[i], 0), on ? UI_BG : UI_TEXT);
    }
}

/* fill the pre-made rows; a new event on top while the list is scrolled down moves the scroll with it, so the rows
   under the finger stay put; a filter change goes back to the top */
static void ev_page_fill(const ui_snap_t *s, bool top)
{
    if (!ev_page) return;
    const app_status_t *st = s->st;
    uev_ctx_t c;
    ev_ctx(&c);
    /* the Drive chip with the drive's own list (kept by the recorder, or read back after a restart): its newest 10 */
    bool own = ev_filter == UEV_DRIVE && sm.ok && sm.nev > 0;
    const sys_event_t *src = st->events;
    uint8_t head = st->event_head, count = st->event_count;
    static sys_event_t own_ring[EVENT_MAX];
    if (own) {
        int k = sm.nev < EVENT_MAX ? sm.nev : EVENT_MAX;
        memset(own_ring, 0, sizeof own_ring);
        memcpy(own_ring, &sm.ev[sm.nev - k], sizeof own_ring[0] * (size_t)k);
        src = own_ring; head = (uint8_t)(k % EVENT_MAX); count = (uint8_t)k;
    }
    uint32_t sig = uev_sig(src, head, count, c.power, st->pattern_windows) ^ (uint32_t)(ev_filter * 0x9E3779B9u) ^ (own ? sm.gen * 2654435761u : 0);
    if (sig == ev_page_sig && !top) return;
    ev_page_sig = sig;
    memcpy(ev_ring, src, sizeof ev_ring);
    int n = ev_rows_n = uev_rows(src, head, count, own ? UEV_ALL : (uev_filter_t)ev_filter, &c, ev_rows, EVENT_MAX);
    int shift = 0;
    if (!top && ev_top_ring >= 0)
        for (int i = 0; i < n; i++) if (ev_rows[i].ring == ev_top_ring) { shift = i; break; }
    for (int i = 0; i < EVENT_MAX; i++) {
        if (i >= n) { lv_obj_add_flag(ev_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(ev_row[i], LV_OBJ_FLAG_HIDDEN);
        ev_row_ring[i] = ev_rows[i].ring;
        ui_set_fmt(ev_row_when[i], "%s   %s", ev_rows[i].when, uev_level_name(ev_ring[ev_rows[i].ring].level));
        ui_set_text(ev_row_text[i], ev_rows[i].text);
        ui_set_color(ev_row_text[i], ev_color(ev_rows[i].tone));
    }
    if (n) lv_obj_add_flag(ev_empty, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(ev_empty, LV_OBJ_FLAG_HIDDEN);
    ev_top_ring = n ? ev_rows[0].ring : -1;
    int total = own ? sm.nev : st->event_count < EVENT_MAX ? st->event_count : EVENT_MAX;
    ui_set_fmt(ev_count, "%d of %d", n, total);
    if (own && sm.past)
        ui_set_fmt(ev_note, "Drive s%06lu, before the last restart: its saved events.", (unsigned long)sm.session);
    else if (own)
        ui_set_fmt(ev_note, "Drive s%06lu: its own events, the newest %d.", (unsigned long)sm.session, n);
    else if (ev_filter == UEV_DRIVE && c.win_ok) {
        char a[16], z[16];
        uev_uptime(a, sizeof a, sm.start_up_ms);
        uev_uptime(z, sizeof z, sm.start_up_ms + (int64_t)sm.dur_s * 1000);
        ui_set_fmt(ev_note, "During drive s%06lu, %s to %s after boot, and its save.", (unsigned long)sm.session, a, z);
    } else if (ev_filter == UEV_DRIVE)
        ui_set_text(ev_note, "No drive has ended yet: car and drive events by type.");
    else ui_set_text(ev_note, "Since the board started; the board keeps the newest 10.");
    lv_obj_update_layout(ev_list);
    if (top) lv_obj_scroll_to_y(ev_list, 0, LV_ANIM_OFF);
    else if (shift && lv_obj_get_scroll_y(ev_list) > 0) lv_obj_scroll_by_bounded(ev_list, 0, -shift * (EV_ROW_H + EV_GAP), LV_ANIM_OFF);
}

static void ev_chip_cb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (f == ev_filter || !ev_st) return;
    ev_filter = f;
    ev_chip_paint();                                      /* shown in this frame */
    ui_snap_t s = { .st = (app_status_t *)ev_st };
    ev_page_fill(&s, true);
}

static void ev_detail_open(int ring);
static void ev_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < ev_rows_n) ev_detail_open(ev_row_ring[i]);
}

static void ev_open(lv_event_t *e)
{
    (void)e;
    if (ev_page || !sm_parent || !ev_st) return;
    ev_page = ev_screen(sm_parent);
    ev_head(ev_page, "Recent events", ev_list_back_cb);
    ev_count = ui_label(ev_page, UI_THEME.f_label, UI_MUTED, "");
    lv_obj_align(ev_count, LV_ALIGN_TOP_RIGHT, -DX, DHY - 7 + (UI_BACK_D - lv_font_get_line_height(UI_THEME.f_label)) / 2);
    int cw = (DW - (UEV_FILTERS - 1) * EV_GAP) / UEV_FILTERS;
    for (int i = 0; i < UEV_FILTERS; i++) {               /* filter chips, 64 px tall */
        lv_obj_t *b = ev_chip[i] = lv_obj_create(ev_page);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, DX + i * (cw + EV_GAP), DY);
        lv_obj_set_size(b, cw, EV_ROW_H);
        lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, ev_chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(ui_label(b, UI_THEME.f_label, UI_TEXT, uev_filter_name((uev_filter_t)i)));
    }
    ev_note = d_txt(ev_page, UI_THEME.f_small, UI_MUTED, "", DX, DY + EV_ROW_H + 6);
    lv_obj_set_width(ev_note, DW);
    lv_label_set_long_mode(ev_note, LV_LABEL_LONG_DOT);
    ev_list = lv_obj_create(ev_page);                     /* plain rows: no shadow, border or opacity layer, scrolls cheaply */
    lv_obj_remove_style_all(ev_list);
    int ly = DY + EV_ROW_H + 26;
    lv_obj_set_pos(ev_list, DX, ly);
    lv_obj_set_size(ev_list, DW + 8, UI_H - 12 - ly);
    lv_obj_set_flex_flow(ev_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ev_list, EV_GAP, 0);
    lv_obj_set_style_pad_right(ev_list, 8, 0);
    lv_obj_set_scroll_dir(ev_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ev_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(ev_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_color(ev_list, lv_color_hex(UI_MUTED), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(ev_list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(ev_list, 4, LV_PART_SCROLLBAR);
    ev_empty = ui_label(ev_list, UI_THEME.f_body, UI_MUTED, "No events of this kind since the board started.");
    lv_obj_set_width(ev_empty, DW);
    lv_label_set_long_mode(ev_empty, LV_LABEL_LONG_WRAP);
    for (int i = 0; i < EVENT_MAX; i++) {
        lv_obj_t *r = ev_row[i] = lv_obj_create(ev_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, DW, EV_ROW_H);
        lv_obj_set_style_bg_color(r, lv_color_hex(UI_CARD), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, st_pressed(), LV_STATE_PRESSED);
        lv_obj_set_style_radius(r, 14, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, ev_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ev_row_when[i] = d_txt(r, UI_THEME.f_small, UI_MUTED, "", 14, 8);
        ev_row_text[i] = d_txt(r, UI_THEME.f_body, UI_TEXT, "", 14, 30);
        lv_obj_set_width(ev_row_text[i], DW - 28 - 24);
        lv_label_set_long_mode(ev_row_text[i], LV_LABEL_LONG_DOT);
        lv_obj_align(ui_label(r, UI_THEME.f_body, UI_MUTED, LV_SYMBOL_RIGHT), LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    }
    ev_top_ring = -1;
    ev_chip_paint();
    ui_snap_t s = { .st = (app_status_t *)ev_st };
    ev_page_fill(&s, true);
}

/* one event in full: the whole text, the time since boot and on the clock, its type and its drive */
static void ev_detail_open(int ring)
{
    if (!ev_page || ev_det || ring < 0 || ring >= EVENT_MAX) return;
    const sys_event_t *e = &ev_ring[ring];
    ev_det = ev_screen(ev_page);
    ev_head(ev_det, "Event", ev_det_back_cb);
    lv_obj_t *col = fbox(ev_det, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_pos(col, DX, DY);
    lv_obj_set_size(col, DW, UI_H - 16 - DY);
    lv_obj_t *c = fcard(col, 0);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_style_border_width(c, 4, 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
    uint32_t tone = e->level == EV_ERROR ? UI_RED : e->level == EV_WARN ? UI_WARN : UI_THEME.dim;
    lv_obj_set_style_border_color(c, lv_color_hex(tone), 0);
    d_lab(c, "WHAT HAPPENED", 0, 0, UI_MUTED);
    lv_obj_t *t = ui_label(c, UI_THEME.f_value, UI_TEXT, e->text);   /* the full text, wrapped: nothing cut */
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    char b[96], w[16];
    unsigned cat = uev_category(e->text);
    ui_set_fmt(ui_label(c, UI_THEME.f_label, e->level == EV_INFO ? UI_MUTED : tone, ""), "%s%s%s", uev_level_name(e->level),
               cat & UEV_CAT_WIFI ? ", Wi-Fi" : "", cat & UEV_CAT_DRIVE ? ", car and drive" : "");
    c = fcard(col, 0);
    lv_obj_set_width(c, LV_PCT(100));
    d_lab(c, "WHEN", 0, 0, UI_MUTED);
    uev_uptime(w, sizeof w, e->uptime_ms);
    ui_set_fmt(ui_label(c, UI_THEME.f_value, UI_TEXT, ""), "%s after boot", w);
    bool before_restart = ev_filter == UEV_DRIVE && sm.ok && sm.nev > 0 && sm.past;   /* another boot's uptime */
    if (before_restart) ui_label(c, UI_THEME.f_label, UI_MUTED, "Before the last restart: time since that start.");
    else if (ev_st && mosaico_clock_known(ev_st)) {
        time_t at = (time_t)(uev_epoch_ms(e->uptime_ms, esp_timer_get_time() / 1000, (int64_t)time(NULL) * 1000) / 1000);
        struct tm tm;
        localtime_r(&at, &tm);
        strftime(b, sizeof b, "%H:%M:%S  %a %d %b %Y", &tm);
        ui_label(c, UI_THEME.f_label, UI_MUTED, b);
    } else ui_label(c, UI_THEME.f_label, UI_MUTED, "Clock not set yet: time since boot only.");
    c = fcard(col, 0);
    lv_obj_set_width(c, LV_PCT(100));
    d_lab(c, "DRIVE", 0, 0, UI_MUTED);
    uint32_t sess = uev_session(e->text);
    if (sess) snprintf(b, sizeof b, "Drive s%06lu (named in the event)", (unsigned long)sess);
    else if (ev_filter == UEV_DRIVE && sm.ok && sm.nev > 0)
        snprintf(b, sizeof b, "Drive s%06lu (its own event list)", (unsigned long)sm.session);
    else if (sm.ok && !sm.past && sm.start_up_ms > 0 && uev_in_drive(e->uptime_ms, sm.start_up_ms, sm.dur_s))
        snprintf(b, sizeof b, "During drive s%06lu, the last drive", (unsigned long)sm.session);
    else if (sm.ok) snprintf(b, sizeof b, "Not during the last drive (s%06lu)", (unsigned long)sm.session);
    else if (ev_st && ev_st->sd_mounted && elm_drive_active()) snprintf(b, sizeof b, "A drive is being recorded (s%06lu); none has ended yet",
                                                                     (unsigned long)ev_st->session_id);
    else snprintf(b, sizeof b, "No drive has ended since the board started");
    lv_obj_t *dl = ui_label(c, UI_THEME.f_body, UI_TEXT, b);
    lv_obj_set_width(dl, LV_PCT(100));
    lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
}

static void sm_fill(void)
{
    lv_obj_clean(sm_p1);
    lv_obj_clean(sm_p2);
    char b[48];
    if (!sm.ok) {
        ui_set_text(sm_hdr, "no drive yet");
        lv_obj_t *l = d_txt(sm_p1, UI_THEME.f_body, UI_MUTED, "No drive has ended since the board started. Hold End drive on "
                                                             "the Home or Drive screen, or switch the car off.", DX, DY);
        lv_obj_set_width(l, DW);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        ev_card_build(sm_p1, DY + 70, 452 - (DY + 70));
    } else {
        if (sm.start_epoch_ms > 1600000000000LL) {
            time_t t0 = (time_t)(sm.start_epoch_ms / 1000), t1 = t0 + sm.dur_s;
            struct tm a, z;
            localtime_r(&t0, &a);
            localtime_r(&t1, &z);
            ui_set_fmt(sm_hdr, "s%06lu, %02d:%02d to %02d:%02d", (unsigned long)sm.session, a.tm_hour, a.tm_min, z.tm_hour, z.tm_min);
        } else ui_set_fmt(sm_hdr, "s%06lu, %ld min", (unsigned long)sm.session, (long)(sm.dur_s / 60));
        /* KPI tiles */
        int w4 = (DW - 24) / 4;
        char e[16];
        const char *eu = ui_econ(sm.l100 > 0 ? 100.0f / sm.l100 : -1, e, sizeof e);
        /* "HARD BRAKES" (111 px at 12 px, letter space 2) did not fit the 88 px inside a tile: the caption is "BRAKES" (61 px)
           and the value says "hard" */
        const char *K[4] = {"DISTANCE", "TIME", "ECONOMY", "BRAKES"};
        char V[4][16], U[4][12];
        snprintf(V[0], 16, "%.1f", (double)sm.km); snprintf(U[0], 12, "km");
        snprintf(V[1], 16, "%ld", (long)(sm.dur_s / 60)); snprintf(U[1], 12, "min");
        snprintf(V[2], 16, "%s", e); snprintf(U[2], 12, "%s", strcmp(eu, "L/100 km") ? eu : "L/100");
        snprintf(V[3], 16, "%d", sm.hard_brakes); snprintf(U[3], 12, "hard");
        for (int k = 0; k < 4; k++) {
            lv_obj_t *c = d_card(sm_p1, DX + k * (w4 + 8), DY, w4, 54, UI_BORDER, false);
            lv_obj_set_style_radius(c, 14, 0);
            lv_obj_set_style_pad_all(c, 8, 0);
            d_lab(c, K[k], 0, 0, UI_MUTED);
            d_val_t v = d_value(c, UI_THEME.f_value, U[k], 0, 16);
            d_value_set(&v, V[k]);
        }
        /* speed and fuel rate over the drive, one chart, fuel on the secondary axis (T1: 36 px shorter, for the events) */
        lv_obj_t *c = d_card(sm_p1, DX, DY + 62, DW, 114, UI_BORDER, false);
        lv_obj_set_style_pad_ver(c, 8, 0);
        d_lab(c, "SPEED KM/H", 0, 0, UI_THEME.section[SEC_SKY]);
        lv_obj_t *fl = d_lab(c, "FUEL L/H", 0, 0, UI_GREEN);
        lv_obj_align(fl, LV_ALIGN_TOP_RIGHT, 0, 0);
        float vmax = 10, fmax = 1;
        for (int i = 0; i < sm.n; i++) { vmax = fmaxf(vmax, sm.spd[i]); fmax = fmaxf(fmax, sm.fuel[i]); }
        lv_obj_t *ch = sm_chart(c, 0, 18, DW - 28, 62, LV_CHART_TYPE_LINE, sm.n, (int)vmax + 5);
        lv_chart_set_axis_range(ch, LV_CHART_AXIS_SECONDARY_Y, 0, (int)(fmax * 10) + 5);
        lv_chart_series_t *ss = lv_chart_add_series(ch, lv_color_hex(UI_THEME.section[SEC_SKY]), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_series_t *sf = lv_chart_add_series(ch, lv_color_hex(UI_GREEN), LV_CHART_AXIS_SECONDARY_Y);
        for (int i = 0; i < sm.n; i++) {
            lv_chart_set_next_value(ch, ss, (int32_t)lroundf(sm.spd[i]));
            lv_chart_set_next_value(ch, sf, (int32_t)lroundf(sm.fuel[i] * 10));
        }
        d_txt(c, UI_THEME.f_small, UI_MUTED, "0 min", 0, 82);
        lv_obj_t *mid = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
        ui_set_fmt(mid, "%ld", (long)(sm.dur_s / 120));
        lv_obj_align(mid, LV_ALIGN_TOP_MID, 0, 82);
        lv_obj_t *end = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
        ui_set_fmt(end, "%ld min", (long)(sm.dur_s / 60));
        lv_obj_align(end, LV_ALIGN_TOP_RIGHT, 0, 82);
        if (sm.past) lv_obj_center(ui_label(c, UI_THEME.f_small, UI_MUTED, "Read back after a restart: the curves are not kept"));
        /* what the AI saw, one block per 10 s (at most 40 blocks), and the class shares */
        c = d_card(sm_p1, DX, DY + 184, DW, 72, UI_BORDER, false);
        lv_obj_set_style_pad_ver(c, 8, 0);
        d_lab(c, "WHAT THE AI SAW, EVERY 10 S", 0, 0, UI_MUTED);
        lv_obj_t *a = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
        ui_set_fmt(a, "%d brakings", sm.brake_events);
        lv_obj_align(a, LV_ALIGN_TOP_RIGHT, 0, 0);
        int nb = sm.dur_s / 10;
        if (nb > 40) nb = 40;
        if (nb < 1) nb = 1;
        int W = DW - 28, bw = W / nb;
        for (int k = 0; k < nb && sm.n > 0; k++) {
            int cls = sm.cls[(int)((long)k * sm.n / nb)];
            ui_rect(c, k * bw, 20, bw > 1 ? bw - 1 : 1, 14, cls >= 0 && cls < 4 ? UI_THEME.cls[cls] : UI_THEME.track, 0);
        }
        static const char *const CN[4] = {"Stopped", "Braking", "Accelerating", "Cruising"};
        int order[4] = {0, 1, 2, 3};
        for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) if (sm.cls_share[order[j]] > sm.cls_share[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        int x = 0;
        for (int i = 0; i < 4; i++) {
            int k = order[i];
            ui_rect(c, x, 44, 9, 9, UI_THEME.cls[k], 2);
            lv_obj_t *l = d_txt(c, UI_THEME.f_small, UI_TEXT, "", x + 14, 40);
            ui_set_fmt(l, "%s %.0f %%", CN[k], sm.cls_share[k] * 100.0);
            lv_obj_update_layout(l);
            x += 14 + lv_obj_get_width(l) + 12;
        }
        ev_card_build(sm_p1, DY + 264, 452 - (DY + 264));
    }
    /* page 2: history from drives.csv */
    d_lab(sm_p2, "HISTORY, LAST DRIVES (OLDEST LEFT)", DX, DY, UI_MUTED);
    if (!sm_h.n) { d_txt(sm_p2, UI_THEME.f_label, UI_MUTED, "No drives in drives.csv yet.", DX, DY + 24); return; }
    float emax = 1, dmax = 1, amax = 1;
    for (int i = 0; i < sm_h.n; i++) { emax = fmaxf(emax, sm_h.d[i].l100); dmax = fmaxf(dmax, sm_h.d[i].km); }
    const char *T[2] = {"L/100 KM PER DRIVE", "KM PER DRIVE"};
    const int NG = 2;
    for (int g = 0; g < NG; g++) {
        lv_obj_t *c = d_card(sm_p2, DX, DY + 20 + g * 106, DW, 98, UI_BORDER, false);
        lv_obj_set_style_pad_ver(c, 8, 0);
        d_lab(c, T[g], 0, 0, UI_MUTED);
        lv_obj_t *ch = sm_chart(c, 0, 18, DW - 28, 62, g == 2 ? LV_CHART_TYPE_LINE : LV_CHART_TYPE_BAR, sm_h.n,
                                g == 0 ? (int)(emax * 10) + 5 : g == 1 ? (int)dmax + 1 : (int)amax + 1);
        lv_chart_series_t *se = lv_chart_add_series(ch, lv_color_hex(g == 0 ? UI_THEME.section[SEC_AMBER] : g == 1 ? UI_THEME.section[SEC_SKY] : UI_GREEN),
                                                    LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < sm_h.n; i++)
            lv_chart_set_next_value(ch, se, g == 0 ? (sm_h.d[i].l100 > 0 ? (int32_t)lroundf(sm_h.d[i].l100 * 10) : 0)
                                            : g == 1 ? (int32_t)lroundf(sm_h.d[i].km)
                                            : 0
                                            );
    }
}

static void summary_create(lv_obj_t *p)
{
    sm_parent = p;
    ev_card = ev_page = ev_det = NULL;
    ev_filter = UEV_ALL;
    d_header(p, "Drive summary");
    sm_hdr = d_right(p, "", UI_MUTED);
    sm_tv = lv_tileview_create(p);
    lv_obj_set_pos(sm_tv, 0, 0);
    lv_obj_set_size(sm_tv, UI_W, UI_H);
    lv_obj_set_style_bg_opa(sm_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(sm_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_background(sm_tv);                        /* the header stays clickable on top */
    sm_p1 = lv_tileview_add_tile(sm_tv, 0, 0, LV_DIR_BOTTOM);
    sm_p2 = lv_tileview_add_tile(sm_tv, 0, 1, LV_DIR_TOP);
    for (int t = 0; t < 2; t++) {
        lv_obj_t *pg = t ? sm_p2 : sm_p1;
        lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        int x = UI_W / 2 - 15;
        ui_rect(pg, x, 456, t ? 6 : 18, 6, t ? UI_THEME.dim : UI_TEXT, LV_RADIUS_CIRCLE);
        ui_rect(pg, x + (t ? 13 : 25), 456, t ? 18 : 6, 6, t ? UI_TEXT : UI_THEME.dim, LV_RADIUS_CIRCLE);
    }
    sm_gen = sm_hgen = UINT32_MAX;
    mosaico_drive_history_request();
}

static void summary_destroy(void)
{
    sm_tv = sm_p1 = sm_p2 = sm_hdr = NULL;
    sm_parent = ev_card = ev_foot = ev_page = ev_det = ev_list = NULL;
    memset(ev_line, 0, sizeof ev_line);
}

static bool sm_due;
static void summary_async(void *arg);
static void summary_update(const ui_snap_t *s)
{
    ev_st = s->st;
    mosaico_drive_get_summary(&sm);
    mosaico_drive_get_history(&sm_h);
    if (sm.gen == sm_gen && sm_h.gen == sm_hgen) {         /* the events change in place, every update (1 s) */
        ev_card_refresh(s);
        ev_page_fill(s, false);
        return;
    }
    sm_gen = sm.gen; sm_hgen = sm_h.gen;
    if (!sm_due) { sm_due = true; lv_async_call(summary_async, NULL); }
    ev_card_refresh(s);
    ev_page_fill(s, false);
}

/* a drive ended (or the history changed): the two pages are rebuilt, after the timed ui job (lv_async) */
static void summary_async(void *arg)
{
    (void)arg;
    sm_due = false;
    if (!sm_p1 || !ev_st) return;
    ui_snap_t snap = { .st = (app_status_t *)ev_st, .now_ms = esp_timer_get_time() / 1000 }, *s = &snap;
    /* sm_fill cleans the pages: the dots go with them, so draw them again */
    sm_fill();
    for (int t = 0; t < 2; t++) {
        lv_obj_t *pg = t ? sm_p2 : sm_p1;
        int x = UI_W / 2 - 15;
        ui_rect(pg, x, 456, t ? 6 : 18, 6, t ? UI_THEME.dim : UI_TEXT, LV_RADIUS_CIRCLE);
        ui_rect(pg, x + (t ? 13 : 25), 456, t ? 18 : 6, 6, t ? UI_TEXT : UI_THEME.dim, LV_RADIUS_CIRCLE);
    }
    ev_card_refresh(s);
    ev_page_fill(s, false);
}

/* ======================= 11. Files (drive logs) ======================= */
/* Everything that touches the file system runs in the board task (mosaico_files.c); this app only asks and shows. */
static lv_obj_t *fl_body;
static int fl_mode;                       /* 0 list, 1 detail */
static uint32_t fl_gen, fl_dgen, fl_id;
EXT_RAM_BSS_ATTR static mf_list_t fl_list;
EXT_RAM_BSS_ATTR static mf_detail_t fl_det;

static void fmt_size(char *b, size_t n, uint32_t bytes)
{
    if (bytes >= 1024u * 1024u) snprintf(b, n, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024u) snprintf(b, n, "%lu KB", (unsigned long)(bytes / 1024));
    else snprintf(b, n, "%lu B", (unsigned long)bytes);
}

static void fmt_when(char *b, size_t n, int64_t epoch_ms)
{
    if (epoch_ms < 1600000000000LL) { snprintf(b, n, "no clock"); return; }
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(b, n, "%Y-%m-%d %H:%M", &tm);
}

static lv_obj_t *wide_button(lv_obj_t *p, const char *t, uint32_t border, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, CW - 16, 52);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_border_width(b, UI_THEME.border_accent, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_radius(b, UI_THEME.radius_button, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    lv_obj_center(ui_label(b, F16, border, t));
    return b;
}

static void fl_show_list(void *arg);
static void fl_show_detail(void *arg);

/* the list's rows are reused for the whole visit: a row's id is looked up when it is tapped */
static uint32_t fl_row_id[MF_MAX];
static int fl_deletable;
static void fl_row_cb(lv_event_t *e) { lv_async_call(fl_show_detail, (void *)(uintptr_t)fl_row_id[(int)(intptr_t)lv_event_get_user_data(e)]); }
static void fl_back_cb(lv_event_t *e) { (void)e; lv_async_call(fl_show_list, NULL); }

static void fl_do_delete_all(void *arg) { (void)arg; mosaico_board_request(MB_FILES_DELETE_ALL, 0); fl_show_list(NULL); }
static void fl_ask_all_again(void *arg)
{
    static char q[128];
    snprintf(q, sizeof q, "Really delete %d drive logs? The drive being written and the learned " LEARNED_STATE " state stay.",
             (int)(intptr_t)arg);
    ui_confirm(q, "Delete all", fl_do_delete_all, NULL);
}
static void fl_all_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm("Delete ALL drive logs on the board? This cannot be undone.", "Continue", fl_ask_all_again, (void *)(intptr_t)fl_deletable);
}

static void fl_do_delete(void *arg) { mosaico_board_request(MB_FILES_DELETE, (uint32_t)(uintptr_t)arg); fl_show_list(NULL); }
static void fl_del_cb(lv_event_t *e)
{
    static char q[96];
    snprintf(q, sizeof q, "Delete drive log s%06lu and all %d of its files? This cannot be undone.", (unsigned long)fl_det.id, fl_det.nf);
    ui_confirm(q, "Delete", fl_do_delete, (void *)(uintptr_t)fl_det.id);
}

static void fl_trash_cb(lv_event_t *e)
{
    static char q[96];
    uint32_t id = fl_row_id[(int)(intptr_t)lv_event_get_user_data(e)];
    snprintf(q, sizeof q, "Delete drive log s%06lu and all its files? This cannot be undone.", (unsigned long)id);
    ui_confirm(q, "Delete", fl_do_delete, (void *)(uintptr_t)id);
}

/* The list page is built once when it is shown (fl_build_list) and updated in place (fl_fill_list): only rows whose
   content changed are rewritten (ui_rows_logic.h), rows are hidden instead of deleted. UI1 measured the old clean +
   refill at 177-185 ms of CPU inside the timed ui job. */
typedef struct { lv_obj_t *r, *t, *z, *d, *pill, *trash; } fl_row_t;
static fl_row_t fl_rows[MF_MAX];
static uint32_t fl_row_sig[MF_MAX];
static int fl_rows_shown, fl_rows_built;
static lv_obj_t *fl_sl, *fl_sb, *fl_flash_l, *fl_msg, *fl_note, *fl_empty, *fl_more, *fl_delall, *fl_foot;

static void fl_row_create(int i)
{
    fl_row_t *w = &fl_rows[i];
    lv_obj_t *r = w->r = lv_obj_create(fl_body);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, CW - 16, 72);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, UI_THEME.radius_row, 0);
    lv_obj_set_style_border_width(r, UI_THEME.border_card, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(UI_BORDER), 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(r, fl_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    w->t = ui_label(r, F20, UI_TEXT, "");
    lv_obj_set_pos(w->t, 14, 10);
    w->z = ui_label(r, F14, UI_MUTED, "");
    lv_obj_set_pos(w->z, 130, 14);
    w->d = ui_label(r, F14, UI_MUTED, "");
    lv_obj_set_pos(w->d, 14, 42);
    w->pill = ui_pill(r, "WRITING", UI_GREEN);
    lv_obj_align(w->pill, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_t *b = w->trash = lv_obj_create(r);           /* 44 px trash button: opens the confirm sheet, never deletes */
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 44, 44);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, fl_trash_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_center(ui_label(b, F16, UI_RED, LV_SYMBOL_TRASH));
    lv_obj_move_to_index(r, (int32_t)lv_obj_get_index(fl_more));   /* rows stay above the footer */
}

static void fl_row_write(int i, const mf_session_t *s)
{
    fl_row_t *w = &fl_rows[i];
    fl_row_id[i] = s->id;
    char when[24], size[16], car[16] = "";
    fmt_when(when, sizeof when, s->start_epoch_ms);
    fmt_size(size, sizeof size, s->bytes);
    if (s->model_year > 1980) snprintf(car, sizeof car, "  MY %d", s->model_year);
    ui_set_fmt(w->t, "s%06lu", (unsigned long)s->id);
    ui_set_text(w->z, size);
    ui_set_fmt(w->d, "%s  %lu rows%s", when, (unsigned long)s->rows, car);
    if (s->current) { lv_obj_remove_flag(w->pill, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(w->trash, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(w->pill, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(w->trash, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_remove_flag(w->r, LV_OBJ_FLAG_HIDDEN);
}

static void fl_show(lv_obj_t *o, bool on) { if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }

static void fl_build_list(void)
{
    lv_obj_clean(fl_body);
    memset(fl_rows, 0, sizeof fl_rows);
    fl_rows_shown = fl_rows_built = 0;
    lv_obj_t *sc = ui_card(fl_body, 0, 0, CW - 16, 70, UI_BORDER);
    ui_caption(sc, "STORAGE");
    fl_flash_l = ui_label(sc, F14, UI_MUTED, "");
    lv_obj_set_pos(fl_flash_l, 0, 42);
    lv_obj_add_flag(fl_flash_l, LV_OBJ_FLAG_HIDDEN);
    fl_sl = ui_label(sc, F14, UI_TEXT, "--");
    lv_obj_align(fl_sl, LV_ALIGN_TOP_RIGHT, 0, -2);
    fl_sb = bar(sc, 0, 26, CW - 16 - 2 * UI_THEME.pad_card, 10, UI_ORANGE, 1000);
    fl_msg = ui_label(fl_body, F16, UI_MUTED, "Reading the log storage...");
    fl_note = ui_label(fl_body, F14, UI_GREEN, "");
    fl_empty = ui_label(fl_body, F16, UI_MUTED, "No drive logs yet.");
    fl_more = ui_label(fl_body, F14, UI_MUTED, "");
    fl_delall = wide_button(fl_body, LV_SYMBOL_TRASH "  Delete all drive logs", UI_RED, fl_all_cb, NULL);
    fl_foot = ui_label(fl_body, F14, UI_MUTED, "Learned " LEARNED_STATE_AND " state, the clock and config.json are never deleted here.");
    lv_obj_set_width(fl_foot, CW - 24);
    lv_label_set_long_mode(fl_foot, LV_LABEL_LONG_WRAP);
    fl_show(fl_note, false); fl_show(fl_empty, false); fl_show(fl_more, false); fl_show(fl_delall, false); fl_show(fl_foot, false);
}

static void fl_fill_list(void)
{
    const mf_list_t *L = &fl_list;
    if (!fl_sl) return;
    /* storage: used bar and free of total */
    lv_obj_t *sc = lv_obj_get_parent(fl_sl);
#if CONFIG_OCM_NAND_LOGS
    lv_obj_set_height(sc, L->nand ? 92 : 70);
    ui_set_text(lv_obj_get_child(sc, 0), L->nand ? "DRIVE LOGS: 128 MB NAND" : "STORAGE");
    if (L->nand && L->flash_total_kb) {
        char a[16], b[16];
        fmt_size(a, sizeof a, L->flash_free_kb * 1024u);
        fmt_size(b, sizeof b, L->flash_total_kb * 1024u);
        ui_set_fmt(fl_flash_l, "State files (flash): %s free of %s", a, b);
        fl_show(fl_flash_l, true);
    } else fl_show(fl_flash_l, false);
#endif
    if (L->total_kb) {
        char a[16], b[16];
        fmt_size(a, sizeof a, L->free_kb * 1024u);
        fmt_size(b, sizeof b, L->total_kb * 1024u);
        ui_set_fmt(fl_sl, "%s free of %s", a, b);
        bar_set(fl_sb, (int)((uint64_t)(L->total_kb - L->free_kb) * 1000 / L->total_kb));
        lv_obj_set_style_bg_color(fl_sb, lv_color_hex(L->free_kb * 10 < L->total_kb ? UI_RED : UI_ORANGE), LV_PART_INDICATOR);
    }
    fl_show(fl_msg, !L->ok);
    if (!L->ok) ui_set_text(fl_msg, L->busy || !L->gen ? "Reading the log storage..." : "No log storage");
    int n = L->ok ? L->n : 0;
    if (L->ok && L->note[0]) ui_set_text(fl_note, L->note);
    fl_show(fl_note, L->ok && L->note[0]);
    fl_show(fl_empty, L->ok && !L->n);
    uint32_t sig[MF_MAX];
    uint8_t act[MF_MAX];
    fl_deletable = 0;
    for (int i = 0; i < n; i++) {
        const mf_session_t *x = &L->s[i];
        fl_deletable += !x->current;
        uint32_t h = urow_mix(UROW_SEED, &x->id, sizeof x->id);
        h = urow_mix(h, &x->bytes, sizeof x->bytes); h = urow_mix(h, &x->rows, sizeof x->rows);
        h = urow_mix(h, &x->start_epoch_ms, sizeof x->start_epoch_ms); h = urow_mix(h, &x->model_year, sizeof x->model_year);
        h = urow_mix(h, &x->current, sizeof x->current);
        sig[i] = h;
    }
    int top = n > fl_rows_shown ? n : fl_rows_shown;
    urow_plan(fl_row_sig, &fl_rows_shown, &fl_rows_built, sig, n, act, MF_MAX);
    for (int i = 0; i < top; i++) {
        if (act[i] & UROW_CREATE) fl_row_create(i);
        if (act[i] & UROW_WRITE) fl_row_write(i, &L->s[i]);
        else if (act[i] == UROW_HIDE) lv_obj_add_flag(fl_rows[i].r, LV_OBJ_FLAG_HIDDEN);
    }
    if (L->ok && L->total > L->n) ui_set_fmt(fl_more, "newest %d of %d shown", L->n, L->total);
    fl_show(fl_more, L->ok && L->total > L->n);
    fl_show(fl_delall, fl_deletable > 0);
    fl_show(fl_foot, L->ok);
}

static void fl_fill_detail(void)
{
    lv_obj_clean(fl_body);
    fl_sl = NULL;                                         /* the list page's objects went with the clean */
    wide_button(fl_body, LV_SYMBOL_LEFT "  All drive logs", UI_ORANGE, fl_back_cb, NULL);
    const mf_detail_t *D = &fl_det;
    lv_obj_t *t = ui_label(fl_body, F28, UI_TEXT, "");
    ui_set_fmt(t, "s%06lu%s", (unsigned long)fl_id, D->current ? "  (recording)" : "");
    if (!D->ok || D->id != fl_id) { ui_label(fl_body, F16, UI_MUTED, "Reading..."); return; }
    char when[24], b[64];
    fmt_when(when, sizeof when, D->start_epoch_ms);
    lv_obj_t *c = ui_card(fl_body, 0, 0, CW - 16, 214, UI_BORDER);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_IGNORE_LAYOUT);
    ui_set_text(row(c, 4, "Start"), when);
    if (D->last_up_ms > D->first_up_ms) {
        int64_t s = (D->last_up_ms - D->first_up_ms) / 1000;
        snprintf(b, sizeof b, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    } else snprintf(b, sizeof b, D->current ? "being written" : "--");
    ui_set_text(row(c, 36, "Duration"), b);
    if (D->km >= 0) snprintf(b, sizeof b, "%.1f km", (double)D->km); else snprintf(b, sizeof b, "--");
    ui_set_text(row(c, 68, "Distance"), b);
    if (D->fuel_l >= 0) snprintf(b, sizeof b, "%.2f L", (double)D->fuel_l); else snprintf(b, sizeof b, "--");
    ui_set_text(row(c, 100, "Fuel"), b);
    snprintf(b, sizeof b, "%lu", (unsigned long)D->rows);
    ui_set_text(row(c, 132, "Rows"), b);
    if (D->model_year > 1980) snprintf(b, sizeof b, "MY %d %.23s", D->model_year, D->protocol);
    else snprintf(b, sizeof b, "%.23s", D->protocol[0] ? D->protocol : "--");
    ui_set_text(row(c, 164, "Car"), b);
    lv_obj_t *fl = ui_label(fl_body, F14, UI_MUTED, "");
    static char files[MF_FILES * 40];
    int k = 0;
    for (int i = 0; i < D->nf && k < (int)sizeof files - 40; i++) {
        char z[16];
        fmt_size(z, sizeof z, D->f[i].bytes);
        k += snprintf(files + k, sizeof files - k, "%s%s  %s", i ? "\n" : "", D->f[i].name, z);
    }
    ui_set_text(fl, k ? files : "no files");
    if (!D->current) wide_button(fl_body, LV_SYMBOL_TRASH "  Delete this drive log", UI_RED, fl_del_cb, NULL);
    else ui_label(fl_body, F14, UI_MUTED, "The drive being written cannot be deleted.");
}

static void fl_show_list(void *arg)
{
    (void)arg;
    if (!fl_body) return;
    fl_mode = 0; fl_gen = 0;
    mosaico_board_request(MB_FILES_SCAN, 0);
    fl_build_list();
    fl_fill_list();
}
static void fl_show_detail(void *arg)
{
    if (!fl_body) return;
    fl_mode = 1; fl_id = (uint32_t)(uintptr_t)arg; fl_dgen = 0;
    memset(&fl_det, 0, sizeof fl_det);
    mosaico_board_request(MB_FILES_DETAIL, fl_id);
    fl_fill_detail();
}

static void files_create(lv_obj_t *p)
{
    ui_header(p, "Files");
    fl_body = lv_obj_create(p);
    lv_obj_remove_style_all(fl_body);
    lv_obj_set_pos(fl_body, UI_MARGIN + 8, Y0);
    lv_obj_set_size(fl_body, CW - 16, UI_H - Y0 - UI_MARGIN);
    lv_obj_set_flex_flow(fl_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(fl_body, 10, 0);
    lv_obj_set_scroll_dir(fl_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(fl_body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(fl_body, LV_OBJ_FLAG_GESTURE_BUBBLE);
    memset(&fl_list, 0, sizeof fl_list);
    fl_show_list(NULL);
}

static void files_destroy(void) { fl_body = fl_sl = NULL; }

/* a new detail (one file read, after a tap) is a rebuild of the page: it runs after the timed ui job (lv_async), not in it */
static void fl_detail_async(void *arg) { (void)arg; if (fl_body && fl_mode == 1) fl_fill_detail(); }

static void files_update(const ui_snap_t *s)
{
    (void)s;
    if (fl_mode == 0) {
        mf_list_t *L = &fl_list;
        mosaico_files_get_list(L);
        if (L->gen != fl_gen && !L->busy) { fl_gen = L->gen; fl_fill_list(); }
    } else {
        mosaico_files_get_detail(&fl_det);
        if (fl_det.gen != fl_dgen && fl_det.id == fl_id) { fl_dgen = fl_det.gen; lv_async_call(fl_detail_async, NULL); }
    }
}

/* ======================= 12. Settings ======================= */
/* Touch targets (S1, owner feedback 2026-11-11): every control is at least ST_H x ST_H, rows are full width and
   targets are ST_GAP apart; where the drawing is smaller (the slider track) the hit area is bigger. A tap restyles
   only the tapped control in that frame; the value applies in RAM at once and the slow work (NVS write, launcher
   rebuild, theme switch) runs later, debounced (ui_settings_set, ui_settings_logic.h). Nothing here rebuilds the page. */
#define ST_W   424                                 /* full row width inside the list (432) less the scroll bar */
#define ST_H   64
#define ST_GAP 10
static lv_obj_t *g_list, *g_ble;
static void settings_fill(void);

/* the raised Row I look: a 1 px top-left highlight, no shadow or gradient, so the list still scrolls cheaply */
static void st_bevel(lv_obj_t *o)
{
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(o, 46, 0);
}
/* pressed: the fill tinted 48/255 toward press_tint (dark: lighter, light: darker) */
static lv_color_t st_pressed(void) { return lv_color_mix(lv_color_hex(UI_THEME.press_tint), lv_color_hex(UI_THEME.track), 48); }

/* ---- HOME APPS ORDER: one full-width row per launcher place, a big Up and Down button on each. A move swaps the two
   rows' labels in place and scrolls the list one row with it, so the moved app stays under the finger. ---- */
static lv_obj_t *g_ord_row[UI_MAX_APPS], *g_ord_tile[UI_MAX_APPS], *g_ord_icon[UI_MAX_APPS], *g_ord_name[UI_MAX_APPS];
static int g_ord_n, g_ord_mark = -1;

static void ord_paint(int pos, int app)
{
    const ocm_app_t *a = ui_app(app);
    ui_set_bg(g_ord_tile[pos], UI_LIGHT ? a->icon_g1 : a->icon_bg);     /* the launcher's tile in each theme */
    if (lv_image_get_src(g_ord_icon[pos]) != (const void *)a->icon52) lv_image_set_src(g_ord_icon[pos], a->icon52);
    ui_set_text(g_ord_name[pos], a->name);
}

static void ord_mark(int pos)                      /* an accent edge on the row of the app that just moved */
{
    if (g_ord_mark >= 0 && g_ord_mark < g_ord_n) lv_obj_set_style_border_color(g_ord_row[g_ord_mark], lv_color_hex(UI_BORDER), 0);
    g_ord_mark = pos;
    lv_obj_set_style_border_color(g_ord_row[pos], lv_color_hex(UI_ORANGE), 0);
}

static void move_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);     /* pos * 2 + (0 up, 1 down) */
    int pos = code / 2, dir = code % 2 ? 1 : -1;
    ui_settings_t s;
    ui_settings_get(&s);
    if (pos >= g_ord_n || !ocm_order_move(&s, pos, dir)) return;
    ui_settings_set(&s);
    ord_paint(pos, s.order[pos]);
    ord_paint(pos + dir, s.order[pos + dir]);
    ord_mark(pos + dir);
    lv_obj_scroll_by_bounded(g_list, 0, -dir * (ST_H + ST_GAP), LV_ANIM_OFF);
}

static lv_obj_t *ord_btn(lv_obj_t *row, int x, const char *sym, int code, bool on)
{
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, 0);
    lv_obj_set_size(b, ST_H, ST_H);
    lv_obj_set_style_radius(b, UI_THEME.radius_button, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? UI_THEME.track : UI_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    if (on) {
        lv_obj_set_style_bg_color(b, st_pressed(), LV_STATE_PRESSED);
        st_bevel(b);
        lv_obj_add_event_cb(b, move_cb, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    } else lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);      /* the top row has no Up, the bottom row no Down */
    lv_obj_center(ui_label(b, F20, on ? UI_TEXT : UI_THEME.dim, sym));
    return b;
}

static int ord_rows(lv_obj_t *p, int y, const ui_settings_t *s)
{
    g_ord_n = s->n;
    g_ord_mark = -1;
    for (int i = 0; i < s->n; i++, y += ST_H + ST_GAP) {
        lv_obj_t *r = lv_obj_create(p);
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, 0, y);
        lv_obj_set_size(r, ST_W, ST_H);
        lv_obj_set_style_radius(r, UI_THEME.radius_row, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(UI_CARD), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(UI_BORDER), 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);   /* a drag on a row scrolls the list */
        g_ord_row[i] = r;
        g_ord_tile[i] = ui_rect(r, 6, 6, 52, 52, 0, 14);
        g_ord_icon[i] = lv_image_create(g_ord_tile[i]);
        lv_obj_set_style_image_recolor(g_ord_icon[i], lv_color_hex(UI_THEME.tile_glyph), 0);
        lv_obj_set_style_image_recolor_opa(g_ord_icon[i], LV_OPA_COVER, 0);
        lv_obj_center(g_ord_icon[i]);
        lv_obj_t *num = ui_label(r, F14, UI_MUTED, "");
        ui_set_fmt(num, "%d", i + 1);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 70, 0);
        g_ord_name[i] = ui_label(r, F20, UI_TEXT, "");
        lv_obj_set_width(g_ord_name[i], ST_W - 98 - 2 * ST_H - 2 * ST_GAP);
        lv_label_set_long_mode(g_ord_name[i], LV_LABEL_LONG_DOT);
        lv_obj_align(g_ord_name[i], LV_ALIGN_LEFT_MID, 96, 0);
        ord_paint(i, s->order[i]);
        ord_btn(r, ST_W - 2 * ST_H - ST_GAP, LV_SYMBOL_UP, i * 2, i > 0);
        ord_btn(r, ST_W - ST_H, LV_SYMBOL_DOWN, i * 2 + 1, i < s->n - 1);
    }
    return y;
}

/* repaint one segment as selected or not: a style change on the segment only */
static void seg_paint(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? UI_ORANGE : UI_THEME.track), 0);
    if (on) lv_obj_set_style_bg_color(b, lv_color_hex(UI_ORANGE), LV_STATE_PRESSED);
    else lv_obj_set_style_bg_color(b, st_pressed(), LV_STATE_PRESSED);
    ui_set_color(lv_obj_get_child(b, 0), on ? UI_BG : UI_TEXT);
}

/* Settings > Model: R.E.V / E.V. The board task switches (ml_select_model, the USB door's MODEL SET path) and keeps the
   choice in NVS; the segment shows the request until the board task answers, then the model in use. The request
   (model_pick, model_poll) is the one the R.E.V and E.V apps' "Use" buttons send. */
static const int MODEL_SEG[2] = {ML_MODEL_REV, ML_MODEL_EV};
static lv_obj_t *g_mseg[2], *g_model;
static int g_model_shown = -1;                             /* segment painted for (-1 none) */

/* the segment and the line under it; called from settings_update (1 s) and right after a tap */
static void model_show(int active, bool ready)
{
    if (!g_model) return;
    int sel = g_model_req >= 0 ? g_model_req : active;
    if (sel != g_model_shown) {
        for (int i = 0; i < 2; i++) seg_paint(g_mseg[i], MODEL_SEG[i] == sel);
        g_model_shown = sel;
    }
    bool rev = active == ML_MODEL_REV;
    int t = ml_selftest_state();
    if (g_model_req >= 0) ui_set_fmt(g_model, "Loading CarMind %s ...", g_model_req == ML_MODEL_REV ? "R.E.V" : "E.V");
    else if (g_model_err[0]) ui_set_fmt(g_model, "Not switched: %s", g_model_err);
    else if (!ready) ui_set_fmt(g_model, "CarMind %s is not loaded.", rev ? "R.E.V" : "E.V");
    else ui_set_fmt(g_model, "CarMind %s %s, self-test %s. %s", rev ? "R.E.V" : "E.V", ml_model_version(active),
                    t > 0 ? "exact" : t == 0 ? "differs" : "not run", rev ? "Learns fuel use in this car." : "Runs as shipped.");
    ui_set_color(g_model, g_model_err[0] ? UI_WARN : UI_MUTED);
}


/* segmented controls: user data = (setting << 8) | option */
enum { SEG_THEME = 1, SEG_UNITS, SEG_OFF, SEG_MODEL, SEG_N };
static const uint16_t OFF_S[] = {0, 10, 20, 60};
static lv_obj_t *g_seg[SEG_N][4];                         /* the segments of each row, painted in place */
static void seg_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e), what = code >> 8, opt = code & 0xFF;
    if (what == SEG_MODEL) {                               /* not a UI setting: the model choice lives in NVS "ml" */
        model_pick(MODEL_SEG[opt]);
        ml_stats_t m;
        ml_get_stats(&m);
        model_show(ml_model_active(), m.ready);
        return;
    }
    if (what <= 0 || what >= SEG_N || opt > 3) return;
    for (int i = 0; i < 4; i++) if (g_seg[what][i]) seg_paint(g_seg[what][i], i == opt);   /* shown in this frame */
    ui_settings_t s;
    ui_settings_get(&s);
    if (what == SEG_THEME) s.theme = (uint8_t)opt;
    else if (what == SEG_UNITS) s.econ_l100 = (uint8_t)opt;
    else if (what == SEG_OFF) s.screen_off_s = OFF_S[opt];
    ui_settings_set(&s);                                   /* RAM now; the save and a theme switch follow, debounced */
}

/* brightness: a slider, 10..100 %. Dragging previews on the panel (at most one command per 50 ms UI tick); the release
   applies the last value and saves it once. */
static lv_obj_t *g_bright_sl, *g_bright_val;
static void bright_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int v = (int)lv_slider_get_value(lv_event_get_target_obj(e));
    if (g_bright_val) ui_set_fmt(g_bright_val, "%d %%", v);
    if (code == LV_EVENT_VALUE_CHANGED) ui_brightness_preview(v);
    else ui_settings_set_brightness(v);                   /* RELEASED or PRESS_LOST */
}

/* ---------- Settings > Clock: the time picker, full screen on lv_layer_top ---------- */
static lv_obj_t *g_clk_time, *g_clk_src;
static lv_obj_t *ck_page, *ck_day, *ck_mon, *ck_year, *ck_hour, *ck_min, *ck_tz, *ck_msg;
static lv_timer_t *ck_timer;
static bool ck_sent;                                      /* a set is queued: ck_tick follows its result */
EXT_RAM_BSS_ATTR static app_status_t ck_st;               /* status copies for the clock lines (too big for the stack) */
static int ck_tz_extra;                                   /* 1: the zone in force is not in the list, row 0 keeps it */
static char ck_tz_cur[48];
static const char *const CK_MON = "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec";

static const char *tz_now(void)
{
    const char *tz = remote_config_get()->timezone;
    return tz[0] ? tz : CONFIG_SYNC_TIMEZONE;
}

static void ck_close(void *arg) { (void)arg; if (ck_page) lv_obj_delete(ck_page); }
static void ck_deleted(lv_event_t *e)
{
    (void)e;
    if (ck_timer) { lv_timer_delete(ck_timer); ck_timer = NULL; }
    ck_page = ck_day = ck_mon = ck_year = ck_hour = ck_min = ck_tz = ck_msg = NULL;
}

static void ck_fill_days(void)
{
    static char buf[32 * 3];
    int y = CLOCK_YEAR_MIN + (int)lv_roller_get_selected(ck_year), m = (int)lv_roller_get_selected(ck_mon) + 1;
    int n = clock_days_in_month(y, m), sel = (int)lv_roller_get_selected(ck_day), k = 0;
    for (int d = 1; d <= n; d++) k += snprintf(buf + k, sizeof buf - k, "%s%d", d > 1 ? "\n" : "", d);
    if ((int)lv_roller_get_option_count(ck_day) == n) return;
    lv_roller_set_options(ck_day, buf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(ck_day, (uint32_t)(sel < n ? sel : n - 1), LV_ANIM_OFF);
}
static void ck_ym_cb(lv_event_t *e) { (void)e; ck_fill_days(); }

static lv_obj_t *ck_roller(lv_obj_t *p, const char *opts, int x, int w, int sel)
{
    lv_obj_t *r = lv_roller_create(p);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_pos(r, x, 112);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, F20, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, UI_THEME.radius_row, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_ORANGE), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(UI_BG), LV_PART_SELECTED);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_roller_set_selected(r, (uint32_t)sel, LV_ANIM_OFF);
    return r;
}

static void ck_show(mclk_result_t r)
{
    if (!ck_msg) return;
    ui_set_text(ck_msg, mosaico_clock_result_text(r));
    ui_set_color(ck_msg, r == MCLK_BUSY || r == MCLK_OK ? UI_MUTED : UI_RED);
}

static void ck_tick(lv_timer_t *t)
{
    (void)t;
    if (!g_list) { lv_async_call(ck_close, NULL); return; }     /* Settings closed (Function key, a rebuild) */
    if (!ck_sent) return;
    mclk_result_t r = mosaico_clock_result();
    ck_show(r);
    if (r == MCLK_OK) lv_async_call(ck_close, NULL);
    if (r != MCLK_BUSY) ck_sent = false;
}

static void ck_save(void)
{
    if (!ck_page) return;
    char tz[48];
    int z = (int)lv_dropdown_get_selected(ck_tz) - ck_tz_extra;
    strlcpy(tz, z < 0 ? ck_tz_cur : clock_tz(z)->tz, sizeof tz);
    ck_sent = mosaico_clock_request(CLOCK_YEAR_MIN + (int)lv_roller_get_selected(ck_year), (int)lv_roller_get_selected(ck_mon) + 1,
                          (int)lv_roller_get_selected(ck_day) + 1, (int)lv_roller_get_selected(ck_hour),
                          (int)lv_roller_get_selected(ck_min), tz);
    ck_show(mosaico_clock_result());
}
static void ck_save_cb(lv_event_t *e) { (void)e; ck_save(); }
static void ck_cancel_cb(lv_event_t *e) { (void)e; lv_async_call(ck_close, NULL); }

static lv_obj_t *ck_button(lv_obj_t *p, const char *t, uint32_t bg, uint32_t fg, int x, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_pos(b, x, 392);
    lv_obj_set_size(b, 196, ST_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(b, F16, fg, t));
    return b;
}

static void style_dropdown(lv_obj_t *d);
static void ck_open(void)
{
    if (ck_page) return;
    ck_sent = false;
    /* start from the time on the clock when there is one, else noon on the first day the picker allows */
    struct tm tm = { .tm_year = CLOCK_YEAR_MIN - 1900, .tm_mon = 0, .tm_mday = 1, .tm_hour = 12 };
    time_t now = time(NULL);
    status_get(&ck_st);
    if (mosaico_clock_known(&ck_st)) localtime_r(&now, &tm);
    int y = tm.tm_year + 1900;
    if (y < CLOCK_YEAR_MIN || y > CLOCK_YEAR_MAX) { y = CLOCK_YEAR_MIN; tm.tm_mon = 0; tm.tm_mday = 1; }

    ck_page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ck_page);
    lv_obj_set_size(ck_page, UI_W, UI_H);
    lv_obj_set_style_bg_color(ck_page, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(ck_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(ck_page, LV_OBJ_FLAG_CLICKABLE);                  /* swallows taps behind it */
    lv_obj_remove_flag(ck_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ck_page, ck_deleted, LV_EVENT_DELETE, NULL);
    /* Back = Cancel: the 44 px chevron at y 22-65 next to the title, its 104 x 104 touch area ends above the rollers (y 112) */
    ui_back(ck_page, 20, 22, 104, UI_THEME.track, UI_ORANGE, ck_cancel_cb);
    at(ui_label(ck_page, F28, UI_TEXT, "Set clock"), 20 + UI_BACK_D + 12, 28);
    at(ui_caption(ck_page, "DAY"), 24, 86);
    at(ui_caption(ck_page, "MONTH"), 96, 86);
    at(ui_caption(ck_page, "YEAR"), 186, 86);
    at(ui_caption(ck_page, "HOUR"), 298, 86);
    at(ui_caption(ck_page, "MIN"), 380, 86);

    static char years[(CLOCK_YEAR_MAX - CLOCK_YEAR_MIN + 1) * 5 + 1], hours[24 * 3], mins[60 * 3];
    int k = 0;
    for (int i = CLOCK_YEAR_MIN; i <= CLOCK_YEAR_MAX; i++) k += snprintf(years + k, sizeof years - k, "%s%d", k ? "\n" : "", i);
    k = 0;
    for (int i = 0; i < 24; i++) k += snprintf(hours + k, sizeof hours - k, "%s%02d", i ? "\n" : "", i);
    k = 0;
    for (int i = 0; i < 60; i++) k += snprintf(mins + k, sizeof mins - k, "%s%02d", i ? "\n" : "", i);
    ck_day  = ck_roller(ck_page, "1", 24, 64, 0);
    ck_mon  = ck_roller(ck_page, CK_MON, 96, 82, tm.tm_mon);
    ck_year = ck_roller(ck_page, years, 186, 100, y - CLOCK_YEAR_MIN);
    ck_hour = ck_roller(ck_page, hours, 298, 70, tm.tm_hour);
    ck_min  = ck_roller(ck_page, mins, 380, 76, tm.tm_min);
    ck_fill_days();
    lv_roller_set_selected(ck_day, (uint32_t)(tm.tm_mday - 1), LV_ANIM_OFF);
    lv_obj_add_event_cb(ck_mon, ck_ym_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ck_year, ck_ym_cb, LV_EVENT_VALUE_CHANGED, NULL);

    at(ui_caption(ck_page, "TIME ZONE"), 24, 262);
    static char zones[48 + 24 * 32];
    strlcpy(ck_tz_cur, tz_now(), sizeof ck_tz_cur);
    int cur = clock_tz_find(ck_tz_cur);
    ck_tz_extra = cur < 0;
    k = 0;
    if (ck_tz_extra) k += snprintf(zones, sizeof zones, "Now: %.36s", ck_tz_cur);
    for (int i = 0; i < clock_tz_count(); i++) k += snprintf(zones + k, sizeof zones - k, "%s%s", k ? "\n" : "", clock_tz(i)->label);
    ck_tz = lv_dropdown_create(ck_page);
    lv_obj_set_pos(ck_tz, 24, 280);
    lv_obj_set_size(ck_tz, UI_W - 48, ST_H);
    lv_dropdown_set_options(ck_tz, zones);
    lv_dropdown_set_selected(ck_tz, (uint32_t)(cur < 0 ? 0 : cur));
    style_dropdown(ck_tz);
    /* each zone in the open list is a 64 px row: the line spacing is part of an option's touch area in lv_dropdown */
    lv_obj_t *zl = lv_dropdown_get_list(ck_tz);
    lv_obj_set_style_text_line_space(zl, ST_H - lv_font_get_line_height(F16), 0);
    lv_obj_set_style_text_font(zl, F16, 0);
    lv_obj_set_style_bg_color(zl, lv_color_hex(UI_CARD), 0);           /* the open list: the palette, not LVGL's white */
    lv_obj_set_style_text_color(zl, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_color(zl, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_bg_color(zl, lv_color_hex(UI_ORANGE), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(zl, lv_color_hex(UI_ORANGE), LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(zl, lv_color_hex(UI_BG), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(zl, lv_color_hex(UI_BG), LV_PART_SELECTED | LV_STATE_PRESSED);
    ck_msg = at(ui_label(ck_page, F14, UI_MUTED, "24-hour time, local to the zone. Wi-Fi time replaces it when it syncs."), 24, 350);
    lv_obj_set_width(ck_msg, UI_W - 48);
    lv_label_set_long_mode(ck_msg, LV_LABEL_LONG_WRAP);
    ck_button(ck_page, "Cancel", UI_THEME.track, UI_TEXT, 24, ck_cancel_cb);
    ck_button(ck_page, "Save", UI_ORANGE, UI_BG, UI_W - 24 - 196, ck_save_cb);
    ck_timer = lv_timer_create(ck_tick, 250, NULL);
}

static void clock_cb(lv_event_t *e)
{
    (void)e;
    if (os_refuse_sheet(OS_SVC_CLOCK_SET, "set the clock")) return;
    ck_open();
}

static void clock_show(const app_status_t *st)
{
    if (!g_clk_time) return;
    clock_src_t src = mosaico_clock_source(st);
    if (src == CLOCK_SRC_NONE) ui_set_text(g_clk_time, "no clock yet");
    else {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char b[40];
        strftime(b, sizeof b, "%H:%M  %a %d %b %Y", &tm);
        ui_set_text(g_clk_time, b);
    }
    ui_set_color(g_clk_time, src == CLOCK_SRC_ESTIMATED ? UI_THEME.estimate : UI_TEXT);
    int z = clock_tz_find(tz_now());
    static const char *const SRC[] = {"Not set", "Estimated, not synced", "Set by hand", "From Wi-Fi"};
    ui_set_fmt(g_clk_src, "%s, zone %.40s", SRC[src], z >= 0 ? clock_tz(z)->label : tz_now());
    ui_set_color(g_clk_src, src == CLOCK_SRC_ESTIMATED ? UI_THEME.estimate : UI_MUTED);
}

static lv_obj_t *g_store, *g_swipe;
static void do_format(void *arg) { (void)arg; mosaico_board_request(MB_FORMAT, 0); }
static void format_again(void *arg)
{
    (void)arg;
#if CONFIG_OCM_NAND_LOGS
    if (nand_store_state() != NAND_ABSENT) {
        ui_confirm("Format the NAND drive store now? Drive logs and summaries cannot be recovered. Learned state and settings "
                   "are kept.", "Format now", do_format, NULL);
        return;
    }
#endif
    ui_confirm("Format now? Drive logs and summaries cannot be recovered. Learned state and settings are kept.", "Format now", do_format, NULL);
}
static void format_cb(lv_event_t *e)
{
    (void)e;
    if (os_refuse_sheet(OS_SVC_FORMAT, "format")) return;
    ui_confirm("Erase all drive logs and summaries on this board?", "Continue", format_again, NULL);
}

static void do_forget(void *arg) { (void)arg; mosaico_board_request(MB_BLE_FORGET, 0); }
static void forget_cb(lv_event_t *e) { (void)e; ui_confirm("Forget the paired OBD adapter? The board then searches for one again.", "Forget", do_forget, NULL); }
static void search_cb(lv_event_t *e) { (void)e; mosaico_board_request(MB_BLE_SEARCH, 0); }

/* a full-height button: ST_H tall, the label in colour c, a 1 px border in c, darker while pressed */
static lv_obj_t *text_btn(lv_obj_t *p, const char *t, int x, int y, int w, uint32_t c, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, ST_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_bg_color(b, st_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, UI_THEME.border_card, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(c), 0);
    lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(b, F16, c, t));
    return b;
}

/* a row of n equal segments across the full width; the selected one is filled with the app accent. out (n entries, or
   NULL) gets the segments. */
static void segmented(lv_obj_t *p, int y, const char *const *opts, int n, int sel, int what, lv_obj_t **out)
{
    int w = (ST_W - (n - 1) * ST_GAP) / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, i * (w + ST_GAP), y);
        lv_obj_set_size(b, w, ST_H);
        lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        st_bevel(b);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)((what << 8) | i));
        lv_obj_center(ui_label(b, F16, UI_TEXT, opts[i]));
        seg_paint(b, i == sel);
        if (out) out[i] = b;
        if (what > 0 && what < SEG_N && i < 4) g_seg[what][i] = b;
    }
}

static void style_dropdown(lv_obj_t *d)
{
    lv_obj_set_style_bg_color(d, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_text_color(d, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, UI_THEME.radius_row, 0);
    lv_obj_set_style_text_font(d, F16, 0);
}

static lv_obj_t *st_note(lv_obj_t *p, int y, const char *t)        /* a muted, wrapped line across the row */
{
    lv_obj_t *l = at(ui_label(p, F14, UI_MUTED, t), 0, y);
    lv_obj_set_width(l, ST_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static void settings_fill(void)
{
    if (!g_list) return;
    lv_obj_clean(g_list);
    g_bright_sl = g_bright_val = g_clk_time = g_clk_src = g_model = NULL;
    memset(g_seg, 0, sizeof g_seg);
    ui_settings_t s;
    ui_settings_get(&s);
    int y = 0;
    at(ui_caption(g_list, "BRIGHTNESS"), 0, y);
    g_bright_val = ui_label(g_list, F14, UI_MUTED, "");
    lv_obj_align(g_bright_val, LV_ALIGN_TOP_RIGHT, -(432 - ST_W), y);
    ui_set_fmt(g_bright_val, "%d %%", s.brightness);
    /* a 12 px track and a 28 px knob, a 64 px tall touch area: 26 px above and below the track */
    g_bright_sl = lv_slider_create(g_list);
    lv_obj_set_pos(g_bright_sl, 26, y + 22 + 26);
    lv_obj_set_size(g_bright_sl, ST_W - 52, 12);
    lv_slider_set_range(g_bright_sl, 10, 100);
    lv_slider_set_value(g_bright_sl, s.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bright_sl, lv_color_hex(UI_THEME.track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_bright_sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bright_sl, lv_color_hex(UI_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_bright_sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_bright_sl, lv_color_hex(UI_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_bright_sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_bright_sl, 8, LV_PART_KNOB);           /* a 28 px knob on the 12 px track */
    lv_obj_set_ext_click_area(g_bright_sl, 26);
    lv_obj_remove_flag(g_bright_sl, LV_OBJ_FLAG_GESTURE_BUBBLE);      /* a fast drag is not the app-to-app swipe */
    lv_obj_add_event_cb(g_bright_sl, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_bright_sl, bright_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(g_bright_sl, bright_cb, LV_EVENT_PRESS_LOST, NULL);
    y += 22 + ST_H + ST_GAP;
    static const char *const TH[] = {"Auto", "Light", "Dark"};
    at(ui_caption(g_list, "THEME (HOME AND APPS)"), 0, y); y += 22;
    segmented(g_list, y, TH, 3, s.theme, SEG_THEME, NULL); y += ST_H + 6;
    st_note(g_list, y, "Auto: Light 06:00 to 17:00 once the clock is set, else Dark."); y += 28;
    static const char *const MD[2] = {"R.E.V", "E.V"};    /* MODEL_SEG order */
    at(ui_caption(g_list, "MODEL"), 0, y); y += 22;
    int act = ml_model_active(), msel = g_model_req >= 0 ? g_model_req : act;
    segmented(g_list, y, MD, 2, MODEL_SEG[0] == msel ? 0 : 1, SEG_MODEL, g_mseg); y += ST_H + 6;
    g_model_shown = msel;
    g_model = st_note(g_list, y, "");
    ml_stats_t mst;
    ml_get_stats(&mst);
    model_show(act, mst.ready);
    y += 46;
    at(ui_caption(g_list, "CLOCK"), 0, y); y += 22;
    g_clk_time = at(ui_label(g_list, F20, UI_TEXT, "--"), 0, y); y += 30;
    g_clk_src = at(ui_label(g_list, F14, UI_MUTED, ""), 0, y);
    lv_obj_set_width(g_clk_src, ST_W);
    lv_label_set_long_mode(g_clk_src, LV_LABEL_LONG_DOT);
    y += 26;
    text_btn(g_list, LV_SYMBOL_EDIT " Set clock", 0, y, ST_W, UI_ORANGE, clock_cb);
    y += ST_H + ST_GAP + 6;
    status_get(&ck_st);
    clock_show(&ck_st);
    static const char *const UN[] = {"km/L", "L/100 km"};
    at(ui_caption(g_list, "ECONOMY UNITS"), 0, y); y += 22;
    segmented(g_list, y, UN, 2, s.econ_l100 ? 1 : 0, SEG_UNITS, NULL); y += ST_H + ST_GAP + 6;
    static const char *const OFF[] = {"Never", "10 s", "20 s", "60 s"};
    int sel = 0;
    for (int i = 0; i < 4; i++) if (OFF_S[i] == s.screen_off_s) sel = i;
    at(ui_caption(g_list, "SCREEN OFF AFTER"), 0, y); y += 22;
    segmented(g_list, y, OFF, 4, sel, SEG_OFF, NULL); y += ST_H + ST_GAP + 6;
    at(ui_caption(g_list, "HOME APPS ORDER"), 0, y); y += 22;
    st_note(g_list, y, "Up and Down move an app one place on Home. The list follows the app."); y += 40;
    y = ord_rows(g_list, y, &s) + 6;
    at(ui_caption(g_list, "OBD ADAPTER (BLUETOOTH)"), 0, y); y += 22;
    g_ble = at(ui_label(g_list, F16, UI_TEXT, "--"), 0, y); y += 28;
    int half = (ST_W - ST_GAP) / 2;
    text_btn(g_list, LV_SYMBOL_BLUETOOTH " Find OBD adapter", 0, y, half, UI_ORANGE, search_cb);   /* not Wi-Fi: the Wi-Fi app searches */
    text_btn(g_list, LV_SYMBOL_CLOSE " Forget adapter", half + ST_GAP, y, half, UI_RED, forget_cb);
    y += ST_H + ST_GAP + 6;
#if CONFIG_OCM_SWIPE_INTENT
    at(ui_caption(g_list, "SMART SWIPE (KEEPS ITSELF IF FASTER)"), 0, y); y += 22;
    g_swipe = st_note(g_list, y, "--");
    ui_set_color(g_swipe, UI_TEXT);
    y += 46;
#endif
    at(ui_caption(g_list, "STORAGE"), 0, y); y += 22;
    g_store = at(ui_label(g_list, F16, UI_TEXT, "--"), 0, y); y += 28;
#if CONFIG_OCM_NAND_LOGS
    bool nand = nand_store_state() != NAND_ABSENT;
    if (nand) { lv_obj_set_width(g_store, ST_W); lv_label_set_long_mode(g_store, LV_LABEL_LONG_WRAP); y += 22; }
#endif
    text_btn(g_list, LV_SYMBOL_WARNING " Format storage", 0, y, ST_W, UI_RED, format_cb);
    y += ST_H + 6;
#if CONFIG_OCM_NAND_LOGS
    st_note(g_list, y, nand ? "Formats the drive store, the 128 MB NAND: drive logs, summaries and drives.csv (about 15 s). "
                              "Learned " LEARNED_STATE " state, clock and config files are on the flash and are not touched." :
                              "Erases drive logs, summaries and drives.csv. Learned " LEARNED_STATE " state, "
                              "clock and config files are copied to RAM and written back.");
    if (nand) y += 20;
#else
    st_note(g_list, y, "Erases drive logs, summaries and drives.csv. Learned " LEARNED_STATE " state, "
                       "clock and config files are copied to RAM and written back.");
#endif
    y += 64;
    at(ui_caption(g_list, "FIRMWARE AND BOARD"), 0, y); y += 22;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    lv_obj_t *info = at(ui_label(g_list, F14, UI_TEXT, ""), 0, y);
    ui_set_fmt(info, "%s\nESP-IDF %s\nESP32-S31 rev %d.%d, %d cores\nFlash %lu MB, PSRAM %u MB\nESP-Mosaico V1.2", CARMIND_FW_STRING,
               esp_get_idf_version(), ci.revision / 100, ci.revision % 100, ci.cores, (unsigned long)(flash >> 20),
               (unsigned)(esp_psram_get_size() >> 20));
    y += 104;
    st_note(g_list, y, "Saved on the board (NVS). Wi-Fi networks: the Wi-Fi app. Function key: home.");
}

static int32_t g_keep_y;                                   /* the list's scroll across a theme rebuild */
static void settings_create(lv_obj_t *p)
{
    ui_header(p, "Settings");
    g_list = lv_obj_create(p);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, UI_MARGIN + 8, Y0);
    lv_obj_set_size(g_list, CW - 16, UI_H - Y0 - UI_MARGIN);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_ACTIVE);
    settings_fill();
    if (ui_theme_reopening() && g_keep_y > 0) {            /* the Theme tap rebuilt the page: stay where the finger was */
        lv_obj_update_layout(g_list);
        lv_obj_scroll_to_y(g_list, g_keep_y, LV_ANIM_OFF);
    }
    g_keep_y = 0;
}

static void settings_destroy(void)
{
    g_keep_y = ui_theme_reopening() && g_list ? lv_obj_get_scroll_y(g_list) : 0;
    g_list = g_ble = g_bright_sl = g_bright_val = g_store = g_swipe = g_clk_time = g_clk_src = g_model = NULL;
    g_mseg[0] = g_mseg[1] = NULL;
    memset(g_seg, 0, sizeof g_seg);
    g_ord_n = 0;
    g_ord_mark = -1;
    g_model_err[0] = 0;                                  /* a refusal is shown once, not on every visit */
    if (ck_page) lv_async_call(ck_close, NULL);
}

static void settings_update(const ui_snap_t *s)
{
    static const char *const BLE[] = {"idle", "searching", "connecting", "discovering", "starting ELM327", "logging", "locked (security on)"};
    const app_status_t *st = s->st;
    if (g_ble) {
        const char *state = st->ble <= BLE_ST_LOCKED ? BLE[st->ble] : "?";
        if (st->adapter_name[0]) ui_set_fmt(g_ble, "%.24s: %s%s", st->adapter_name, state, st->adapter_saved ? ", paired" : "");
        else ui_set_fmt(g_ble, "%s%s", state, st->adapter_saved ? ", paired" : ", none paired");
    }
#if CONFIG_OCM_SWIPE_INTENT
    if (g_swipe) { char b[96]; ui_swipe_status(b, sizeof b); ui_set_text(g_swipe, b); }
#endif
    if (g_store) {
#if CONFIG_OCM_NAND_LOGS
        if (s->fat.ok && s->fat.nand)
            ui_set_fmt(g_store, "Drive logs (NAND): %.1f of %.1f MB free\nState files (flash): %lu of %lu KB free",
                       s->fat.free_kb / 1024.0, s->fat.total_kb / 1024.0, (unsigned long)s->fat.flash_free_kb,
                       (unsigned long)s->fat.flash_total_kb);
        else
#endif
        if (s->fat.ok) ui_set_fmt(g_store, "%lu of %lu KB free", (unsigned long)s->fat.free_kb, (unsigned long)s->fat.total_kb);
        else ui_set_text(g_store, "measuring");
    }
    clock_show(st);
    model_poll(false);
    model_show(s->model, s->ml.ready);
}

/* ======================= 13. Wi-Fi ======================= */
/* The networks the board uses to set its clock (and, paired with the desktop app, to send data while parked): the
   status and "Sync clock now", "Search for networks" (one active scan in the board task, the merged list below it),
   joining a network from the list (a password page with a keyboard for a secured one, a confirm for an open one), and
   the saved list with a Forget on each. The lists are copies the board task makes (wifi_creds belongs to the Wi-Fi
   task); names are shown on this screen only and never logged, a typed password goes to the board task and is wiped. */
#include "esp_log.h"
static lv_obj_t *w_list, *w_state, *w_clock, *w_clock_note, *w_note, *w_rows, *w_det[MB_WIFI_NETS];
static lv_obj_t *w_search_btn, *w_search_lbl, *w_scan_msg, *w_join_msg, *w_found, *w_saved_cap;
static mosaico_wifi_list_t w_copy;                       /* LVGL task: the last copy shown */
static uint32_t w_gen;
static int64_t w_asked_ms;
static char w_forget_ssid[33];
EXT_RAM_BSS_ATTR static mosaico_wifi_scan_t w_scan;      /* LVGL task: the last search list copied */
EXT_RAM_BSS_ATTR static ws_row_t w_shown[WS_ROWS_MAX];   /* the rows on screen: taps read these, a new search may be running */
static int w_shown_n;
static uint32_t w_scan_gen = UINT32_MAX, w_join_gen;
static char w_open_ssid[33];                             /* the open network a confirm sheet asks about */
static lv_style_t w_st_row, w_st_bar_on, w_st_bar_off;
static bool w_st_ready;
static uint32_t w_clock_gen;

static void w_do_forget(void *arg)
{
    (void)arg;
    if (!mosaico_board_wifi_forget(w_forget_ssid)) ui_confirm("The board is busy. Try again in a moment.", "OK", NULL, NULL);
}

static void w_forget_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= w_copy.n) return;
    strlcpy(w_forget_ssid, w_copy.net[i].ssid, sizeof w_forget_ssid);
    static char q[96];
    char name[40];
    ws_display_name(w_forget_ssid, name, sizeof name);
    snprintf(q, sizeof q, "Forget %.36s? The board stops using it to set the clock.", name);
    ui_confirm(q, "Forget", w_do_forget, NULL);
}

#if CONFIG_OCM_WIFI_CLOCK_SYNC
static void w_sync_cb(lv_event_t *e)
{
    (void)e;
    if (!w_copy.n) {                                     /* nothing to connect to: say so at once */
        ui_set_text(w_clock_note, "No saved network yet: search and join one first.");
        ui_set_color(w_clock_note, UI_WARN);
        return;
    }
    ui_set_color(w_clock_note, UI_MUTED);
    ui_set_text(w_clock_note, mosaico_board_request(MB_WIFI_NOW, 0) ? "Asking the Wi-Fi task..." : "The board is busy. Try again in a moment.");
}
#endif

static void w_show_saved(void *arg) { (void)arg; if (w_saved_cap) lv_obj_scroll_to_view(w_saved_cap, LV_ANIM_ON); }

static void w_detail(int i)                              /* the line under a network name, changed in place */
{
    lv_obj_t *d = w_det[i];
    if (w_copy.net[i].fails) ui_set_fmt(d, "%u failed connect%s", w_copy.net[i].fails, w_copy.net[i].fails == 1 ? "" : "s");
    else if (w_copy.net[i].rssi > -127) ui_set_fmt(d, "seen at %d dBm", w_copy.net[i].rssi);
    else ui_set_text(d, "not seen since the start");
    ui_set_color(d, w_copy.net[i].fails ? UI_WARN : UI_MUTED);
}

static void w_fill_rows(void)
{
    lv_obj_clean(w_rows);                                  /* at most 8 rows, only when the saved list changed */
    memset(w_det, 0, sizeof w_det);
    if (!w_copy.n) {
        st_note(w_rows, 0, "No network saved yet.");
        lv_obj_set_height(w_rows, 24);
        return;
    }
    int y = 0;
    for (int i = 0; i < w_copy.n; i++, y += ST_H + ST_GAP) {
        lv_obj_t *r = lv_obj_create(w_rows);
        lv_obj_remove_style_all(r);
        lv_obj_add_style(r, &w_st_row, 0);
        lv_obj_set_pos(r, 0, y);
        lv_obj_set_size(r, ST_W, ST_H);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        char name[40];
        ws_display_name(w_copy.net[i].ssid, name, sizeof name);
        lv_obj_t *n = ui_label(r, F16, UI_TEXT, name);
        lv_obj_set_width(n, ST_W - 140 - 28);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(n, 14, 10);
        w_det[i] = ui_label(r, F14, UI_MUTED, "");
        w_detail(i);
        lv_obj_set_pos(w_det[i], 14, 36);
        lv_obj_t *b = lv_button_create(r);
        lv_obj_set_pos(b, ST_W - 140, 0);
        lv_obj_set_size(b, 140, ST_H);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
        lv_obj_set_style_bg_color(b, st_pressed(), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, UI_THEME.border_card, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_RED), 0);
        lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, w_forget_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(ui_label(b, F16, UI_RED, LV_SYMBOL_CLOSE " Forget"));
    }
    lv_obj_set_height(w_rows, y - ST_GAP);
}

/* ---------- Wi-Fi > password page: full screen on lv_layer_top (the clock picker's pattern) ----------
   480 x 480: title 10-36, the masked field 42-106 (392 x 64) with the show/hide key (64 x 64), the message 110-144,
   the keyboard 146-400 (4 rows, 6 px apart: letter keys 44 x 59 px), Cancel and Join 408-472 (228 x 64). The maps
   carry every printable ASCII character: letters, then two symbol pages ("123", "#+="). */
static lv_obj_t *pw_page, *pw_ta, *pw_kb, *pw_msg, *pw_eye, *pw_join;
static char pw_ssid[33];
static bool pw_saved, pw_sent;
static lv_timer_t *pw_close_timer;

#define KB_123 "123"
#define KB_SYM "#+="
static const char *const PW_LC[] = { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
                                     "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "\n",
                                     "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
                                     KB_123, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, ".", "" };
static const char *const PW_UC[] = { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
                                     "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "\n",
                                     "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
                                     KB_123, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, ".", "" };
static const char *const PW_S1[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
                                     "!", "@", "#", "$", "%", "&", "*", "(", ")", "_", "\n",
                                     KB_SYM, "-", "+", "=", "/", ":", ";", "'", LV_SYMBOL_BACKSPACE, "\n",
                                     "abc", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, ",", "?", ".", "" };
static const char *const PW_S2[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
                                     "[", "]", "{", "}", "<", ">", "^", "~", "`", "|", "\n",
                                     KB_123, "\\", "\"", "'", "!", "?", ",", ".", LV_SYMBOL_BACKSPACE, "\n",
                                     "abc", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, "_", "" };
#define KC (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED)   /* a mode key */
#define KD LV_BUTTONMATRIX_CTRL_CHECKED                                    /* backspace and the cursor keys: repeat */
/* widths in units, 20 per row: a letter is 2 (44 px), so the long keys are 3 */
static const lv_buttonmatrix_ctrl_t PW_CTRL_L[] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                                   2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                                   KC | 3, 2, 2, 2, 2, 2, 2, 2, KD | 3,
                                                   KC | 3, KD | 3, 8, KD | 3, 3 };
static const lv_buttonmatrix_ctrl_t PW_CTRL_S1[] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                                    2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                                    KC | 3, 2, 2, 2, 2, 2, 2, 2, KD | 3,
                                                    KC | 3, KD | 2, 6, KD | 2, 2, 2, 3 };
static const lv_buttonmatrix_ctrl_t PW_CTRL_S2[] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                                    2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                                    KC | 3, 2, 2, 2, 2, 2, 2, 2, KD | 3,
                                                    KC | 3, KD | 3, 8, KD | 3, 3 };

static void pw_ram(const char *when)
{
    ESP_LOGI("wifi_ui", "password page %s: internal RAM %u B free, lowest %u B, largest block %u B", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* LVGL frees its text buffers without clearing them: overwrite what the field holds before the page goes */
static void pw_wipe_field(void)
{
    if (!pw_ta) return;
    char *t = (char *)lv_textarea_get_text(pw_ta);       /* the password copy in password mode, else the label's text */
    if (t) ws_wipe(t, strlen(t));
    char *l = lv_label_get_text(lv_textarea_get_label(pw_ta));
    if (l) ws_wipe(l, strlen(l));
}

static void pw_close(void *arg) { (void)arg; if (pw_page) { pw_wipe_field(); lv_obj_delete(pw_page); } }
static void pw_deleted(lv_event_t *e)
{
    (void)e;
    if (pw_close_timer) { lv_timer_delete(pw_close_timer); pw_close_timer = NULL; }
    pw_page = pw_ta = pw_kb = pw_msg = pw_eye = pw_join = NULL;
    pw_sent = false;
    ws_wipe(pw_ssid, sizeof pw_ssid);
    pw_ram("closed");
}
static void pw_close_tick(lv_timer_t *t) { (void)t; pw_close_timer = NULL; lv_async_call(pw_close, NULL); }

static void pw_say(const char *t, uint32_t c)
{
    if (!pw_msg) return;
    ui_set_text(pw_msg, t);
    ui_set_color(pw_msg, c);
}

static void pw_busy(bool on)
{
    if (!pw_join) return;
    if (on) lv_obj_add_state(pw_join, LV_STATE_DISABLED); else lv_obj_remove_state(pw_join, LV_STATE_DISABLED);
}

static void pw_cancel_cb(lv_event_t *e) { (void)e; lv_async_call(pw_close, NULL); }

static void pw_join_cb(lv_event_t *e)
{
    (void)e;
    if (!pw_page || pw_sent) return;
    const char *pw = lv_textarea_get_text(pw_ta);
    wv_result_t v = wv_check(pw_ssid, pw, true, pw_saved);
    if (v != WV_OK) { pw_say(wv_text(v), UI_RED); return; }
    if (!pw_saved && w_copy.n >= MB_WIFI_NETS) { pw_say(wj_text(WJ_FULL), UI_RED); return; }
    pw_sent = mosaico_board_wifi_join(pw_ssid, pw[0] ? pw : NULL);
    if (!pw_sent) { pw_say(wj_text(WJ_BUSY), UI_RED); return; }
    pw_say(wj_text(WJ_WAIT), UI_MUTED);
    pw_busy(true);
}

static void pw_eye_cb(lv_event_t *e)
{
    (void)e;
    bool hidden = lv_textarea_get_password_mode(pw_ta);
    lv_textarea_set_password_mode(pw_ta, !hidden);
    lv_label_set_text(lv_obj_get_child(pw_eye, 0), hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

/* the mode keys this page adds ("123", "#+="); everything else is the keyboard's own handler */
static void pw_kb_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_current_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    const char *t = id == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(kb, id);
    if (t && !strcmp(t, KB_123)) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_SPECIAL); return; }
    if (t && !strcmp(t, KB_SYM)) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1); return; }
    lv_keyboard_def_event_cb(e);
}

/* a join answer for the network on this page (from wifi_update) */
static void pw_result(const mosaico_wifi_join_t *j)
{
    if (!pw_page || !pw_sent || strcmp(j->ssid, pw_ssid)) return;
    if (j->result == WJ_WAIT) return;
    pw_sent = false;
    if (j->result == WJ_CONNECTED) {
        pw_say(wj_text(WJ_CONNECTED), UI_GREEN);
        if (!pw_close_timer) { pw_close_timer = lv_timer_create(pw_close_tick, 1500, NULL); lv_timer_set_repeat_count(pw_close_timer, 1); }
        return;
    }
    if (j->result == WJ_FULL) {
        lv_async_call(pw_close, NULL);
        ui_confirm(wj_text(WJ_FULL), "Show saved", w_show_saved, NULL);
        return;
    }
    pw_say(wj_text((wj_result_t)j->result), UI_RED);
    pw_busy(false);
}

static lv_obj_t *pw_button(const char *t, uint32_t bg, uint32_t fg, int x, int y, int w, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(pw_page);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, ST_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, st_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(b, F16, fg, t));
    return b;
}

static void pw_open(const ws_row_t *row)
{
    if (pw_page) return;
    strlcpy(pw_ssid, row->ssid, sizeof pw_ssid);
    pw_saved = row->saved;
    pw_sent = false;
    pw_page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pw_page);
    lv_obj_set_size(pw_page, UI_W, UI_H);
    lv_obj_set_style_bg_color(pw_page, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(pw_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(pw_page, LV_OBJ_FLAG_CLICKABLE);                  /* swallows taps behind it */
    lv_obj_remove_flag(pw_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(pw_page, pw_deleted, LV_EVENT_DELETE, NULL);
    /* Back (= Cancel): the chevron left of the field, its touch area from the corner down to the message line */
    ui_back(pw_page, 8, 42 + (ST_H - UI_BACK_D) / 2, 106, UI_THEME.track, UI_ORANGE, pw_cancel_cb);

    char name[40];
    ws_display_name(row->ssid, name, sizeof name);
    lv_obj_t *t = ui_label(pw_page, F20, UI_TEXT, "");
    ui_set_fmt(t, LV_SYMBOL_WIFI " Join %s", name);
    lv_obj_set_pos(t, 8, 10);
    lv_obj_set_width(t, UI_W - 16);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);

    pw_ta = lv_textarea_create(pw_page);
    lv_obj_set_pos(pw_ta, 8 + UI_BACK_D + 8, 42);
    lv_obj_set_size(pw_ta, UI_W - 8 - UI_BACK_D - 8 - 8 - 64 - 8, ST_H);
    lv_textarea_set_one_line(pw_ta, true);
    lv_textarea_set_password_mode(pw_ta, true);
    lv_textarea_set_max_length(pw_ta, 64);                             /* WIFI-ADD's limit; every key is one byte */
    lv_textarea_set_placeholder_text(pw_ta, pw_saved ? "Saved password" : "Password");
    lv_obj_set_style_text_font(pw_ta, F20, 0);
    lv_obj_set_style_text_color(pw_ta, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_color(pw_ta, lv_color_hex(UI_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(pw_ta, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_bg_opa(pw_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pw_ta, 1, 0);
    lv_obj_set_style_border_color(pw_ta, lv_color_hex(UI_ORANGE), 0);
    lv_obj_set_style_radius(pw_ta, UI_THEME.radius_row, 0);
    lv_obj_set_style_pad_hor(pw_ta, 14, 0);
    lv_obj_set_style_pad_ver(pw_ta, (ST_H - lv_font_get_line_height(F20)) / 2 - 1, 0);
    lv_obj_set_style_bg_color(pw_ta, lv_color_hex(UI_ORANGE), LV_PART_CURSOR);
    lv_obj_set_style_border_color(pw_ta, lv_color_hex(UI_ORANGE), LV_PART_CURSOR);
    lv_obj_add_state(pw_ta, LV_STATE_FOCUSED);                         /* shows the cursor */

    pw_eye = lv_button_create(pw_page);
    lv_obj_set_pos(pw_eye, UI_W - 8 - 64, 42);
    lv_obj_set_size(pw_eye, 64, ST_H);
    lv_obj_set_style_bg_color(pw_eye, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_bg_color(pw_eye, st_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(pw_eye, UI_THEME.radius_row, 0);
    lv_obj_set_style_shadow_width(pw_eye, 0, 0);
    lv_obj_add_event_cb(pw_eye, pw_eye_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(pw_eye, F20, UI_TEXT, LV_SYMBOL_EYE_OPEN));

    pw_msg = ui_label(pw_page, F14, UI_MUTED, pw_saved ? "Saved: leave it empty to use the saved password, or type a new one."
                                                      : "8 to 64 characters. The eye shows what you typed.");
    lv_obj_set_pos(pw_msg, 8, 110);
    lv_obj_set_width(pw_msg, UI_W - 16);
    lv_label_set_long_mode(pw_msg, LV_LABEL_LONG_WRAP);

    pw_kb = lv_keyboard_create(pw_page);
    lv_keyboard_set_map(pw_kb, LV_KEYBOARD_MODE_TEXT_LOWER, PW_LC, PW_CTRL_L);
    lv_keyboard_set_map(pw_kb, LV_KEYBOARD_MODE_TEXT_UPPER, PW_UC, PW_CTRL_L);
    lv_keyboard_set_map(pw_kb, LV_KEYBOARD_MODE_SPECIAL, PW_S1, PW_CTRL_S1);
    lv_keyboard_set_map(pw_kb, LV_KEYBOARD_MODE_USER_1, PW_S2, PW_CTRL_S2);
    lv_keyboard_set_mode(pw_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_popovers(pw_kb, false);
    lv_obj_remove_event_cb(pw_kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(pw_kb, pw_kb_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_keyboard_set_textarea(pw_kb, pw_ta);
    lv_obj_align(pw_kb, LV_ALIGN_TOP_LEFT, 0, 146);   /* lv_keyboard_create aligns it BOTTOM_MID: set_pos kept that, 146 px below */
    lv_obj_set_size(pw_kb, UI_W, 400 - 146);
    lv_obj_set_style_bg_color(pw_kb, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(pw_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pw_kb, 0, 0);
    lv_obj_set_style_radius(pw_kb, 0, 0);
    lv_obj_set_style_pad_hor(pw_kb, 2, 0);
    lv_obj_set_style_pad_ver(pw_kb, 0, 0);
    lv_obj_set_style_pad_row(pw_kb, 6, 0);
    lv_obj_set_style_pad_column(pw_kb, 4, 0);
    lv_obj_set_style_text_font(pw_kb, F20, LV_PART_ITEMS);
    lv_obj_set_style_text_color(pw_kb, lv_color_hex(UI_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(pw_kb, lv_color_hex(UI_THEME.track), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(pw_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(pw_kb, st_pressed(), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(pw_kb, lv_color_hex(UI_CARD), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(pw_kb, lv_color_hex(UI_ORANGE), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(pw_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(pw_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(pw_kb, 8, LV_PART_ITEMS);

    pw_button("Cancel", UI_THEME.track, UI_TEXT, 8, 408, (UI_W - 24) / 2, pw_cancel_cb);
    pw_join = pw_button("Join", UI_ORANGE, UI_BG, 8 + (UI_W - 24) / 2 + 8, 408, (UI_W - 24) / 2, pw_join_cb);
    pw_ram("open");
}

/* ---------- the search list ---------- */
static void w_bars(lv_obj_t *p, int x, int bars)          /* 4 bars, 6 px wide, 8/14/20/26 px tall, on one baseline */
{
    for (int k = 0; k < 4; k++) {
        lv_obj_t *b = lv_obj_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_add_style(b, k < bars ? &w_st_bar_on : &w_st_bar_off, 0);
        lv_obj_set_size(b, 6, 8 + 6 * k);
        lv_obj_set_pos(b, x + 10 * k, 45 - (8 + 6 * k));
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void w_lock(lv_obj_t *p, int x)                    /* a padlock: a 10 x 10 ring above a 14 x 11 body */
{
    lv_obj_t *s = lv_obj_create(p);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 10, 12);
    lv_obj_set_pos(s, x + 2, 20);
    lv_obj_set_style_border_width(s, 2, 0);
    lv_obj_set_style_border_color(s, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_radius(s, 5, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 14, 11);
    lv_obj_set_pos(b, x, 28);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
}

static void w_join_now(void *arg)                          /* the confirm sheet of an open network said Join */
{
    (void)arg;
    bool saved = false;
    for (int i = 0; i < w_copy.n; i++) if (!strcmp(w_copy.net[i].ssid, w_open_ssid)) saved = true;
    if (!mosaico_board_wifi_join(w_open_ssid, saved ? NULL : "")) ui_confirm(wj_text(WJ_BUSY), "OK", NULL, NULL);
}

static void w_found_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= w_shown_n) return;
    const ws_row_t *r = &w_shown[i];
    if (!r->joinable) { ui_confirm(wv_text(WV_SSID_CTRL), "OK", NULL, NULL); return; }
    if (!r->saved && w_copy.n >= MB_WIFI_NETS) { ui_confirm(wj_text(WJ_FULL), "Show saved", w_show_saved, NULL); return; }
    if (r->secure) { pw_open(r); return; }
    strlcpy(w_open_ssid, r->ssid, sizeof w_open_ssid);
    static char q[112];
    char name[40];
    ws_display_name(r->ssid, name, sizeof name);
    snprintf(q, sizeof q, "Join %.36s? It is open: no password, and anyone nearby can read what it carries.", name);
    ui_confirm(q, "Join", w_join_now, NULL);
}

static void w_fill_found(void)
{
    lv_obj_clean(w_found);                                 /* only when a new search list arrives */
    w_shown_n = w_scan.n;
    memcpy(w_shown, w_scan.row, (size_t)w_shown_n * sizeof w_shown[0]);
    int y = 0;
    for (int i = 0; i < w_shown_n; i++, y += ST_H + ST_GAP) {
        const ws_row_t *w = &w_shown[i];
        lv_obj_t *r = lv_obj_create(w_found);
        lv_obj_remove_style_all(r);
        lv_obj_add_style(r, &w_st_row, 0);
        lv_obj_set_style_bg_color(r, st_pressed(), LV_STATE_PRESSED);
        lv_obj_set_pos(r, 0, y);
        lv_obj_set_size(r, ST_W, ST_H);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, w_found_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char name[40];
        ws_display_name(w->ssid, name, sizeof name);
        lv_obj_t *n = ui_label(r, F16, UI_TEXT, name);
        lv_obj_set_width(n, ST_W - 14 - 96);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(n, 14, 10);
        const char *sub = !w->joinable ? "Can't join: control characters in the name"
                        : w->saved ? (w->secure ? "Saved, secured" : "Saved, open")
                        : w->secure ? "Secured: tap to type the password" : "Open: no password";
        lv_obj_t *d = ui_label(r, F14, !w->joinable ? UI_WARN : w->saved ? UI_GREEN : UI_MUTED, sub);
        lv_obj_set_pos(d, 14, 36);
        if (w->secure) w_lock(r, ST_W - 82);
        w_bars(r, ST_W - 50, ws_bars(w->rssi));
    }
    lv_obj_set_height(w_found, y ? y - ST_GAP : 0);
}

static void w_scan_show(void)
{
    char b[96];
    ws_state_text(w_scan.state, w_scan.n, w_scan.n_seen, b, sizeof b);
    ui_set_text(w_scan_msg, b);
    ui_set_color(w_scan_msg, w_scan.state == WS_ST_FAILED || w_scan.state == WS_ST_DRIVING ? UI_WARN : UI_MUTED);
    bool busy = w_scan.state == WS_ST_SEARCHING;
    ui_set_text(w_search_lbl, busy ? LV_SYMBOL_WIFI " Searching..." : LV_SYMBOL_WIFI " Search for networks");
    if (busy) lv_obj_add_state(w_search_btn, LV_STATE_DISABLED); else lv_obj_remove_state(w_search_btn, LV_STATE_DISABLED);
    if (w_scan.state == WS_ST_DONE) w_fill_found();
    else if (w_scan.state != WS_ST_SEARCHING) { lv_obj_clean(w_found); lv_obj_set_height(w_found, 0); w_shown_n = 0; }
}

static void w_search_cb(lv_event_t *e)
{
    (void)e;
    if (w_scan.state == WS_ST_SEARCHING) return;
    mosaico_board_wifi_search();                           /* the list arrives with wifi_update */
}

static void w_join_show(const mosaico_wifi_join_t *j)
{
    pw_result(j);
    char name[40];
    ws_display_name(j->ssid, name, sizeof name);
    if (j->result == WJ_NONE) ui_set_text(w_join_msg, "");
    else if (j->result == WJ_WAIT) ui_set_fmt(w_join_msg, "Joining %s...", name);
    else ui_set_fmt(w_join_msg, "%s: %s", name, wj_text((wj_result_t)j->result));
    ui_set_color(w_join_msg, j->result == WJ_CONNECTED ? UI_GREEN : j->result == WJ_WAIT ? UI_MUTED : UI_WARN);
}

static void w_styles(void)                                 /* no object uses them now: the app was closed */
{
    if (w_st_ready) { lv_style_reset(&w_st_row); lv_style_reset(&w_st_bar_on); lv_style_reset(&w_st_bar_off); }
    w_st_ready = true;
    lv_style_init(&w_st_row);
    lv_style_set_radius(&w_st_row, UI_THEME.radius_row);
    lv_style_set_bg_color(&w_st_row, lv_color_hex(UI_CARD));
    lv_style_set_bg_opa(&w_st_row, LV_OPA_COVER);
    lv_style_set_border_width(&w_st_row, 1);
    lv_style_set_border_color(&w_st_row, lv_color_hex(UI_BORDER));
    lv_style_init(&w_st_bar_on);
    lv_style_set_bg_color(&w_st_bar_on, lv_color_hex(UI_TEXT));
    lv_style_set_bg_opa(&w_st_bar_on, LV_OPA_COVER);
    lv_style_set_radius(&w_st_bar_on, 1);
    lv_style_init(&w_st_bar_off);
    lv_style_set_bg_color(&w_st_bar_off, lv_color_hex(UI_THEME.track));
    lv_style_set_bg_opa(&w_st_bar_off, LV_OPA_COVER);
    lv_style_set_radius(&w_st_bar_off, 1);
}

static lv_obj_t *w_wrapped(lv_obj_t *p, uint32_t c, const char *t)
{
    lv_obj_t *l = ui_label(p, F14, c, t);
    lv_obj_set_width(l, ST_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static void wifi_create(lv_obj_t *p)
{
    w_styles();
    ui_header(p, "Wi-Fi");
    w_list = lv_obj_create(p);
    lv_obj_remove_style_all(w_list);
    lv_obj_set_pos(w_list, UI_MARGIN + 8, Y0);
    lv_obj_set_size(w_list, CW - 16, UI_H - Y0 - UI_MARGIN);
    lv_obj_set_scroll_dir(w_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(w_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(w_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(w_list, 8, 0);
    ui_caption(w_list, "STATUS");
    w_state = ui_label(w_list, F20, UI_TEXT, "--");
    lv_obj_set_width(w_state, ST_W);
    lv_label_set_long_mode(w_state, LV_LABEL_LONG_WRAP);
    w_clock = ui_label(w_list, F14, UI_MUTED, "");
#if CONFIG_OCM_WIFI_CLOCK_SYNC
    lv_obj_t *sb = text_btn(w_list, LV_SYMBOL_REFRESH " Sync clock now", 0, 0, ST_W, UI_ORANGE, w_sync_cb);
    lv_obj_set_style_margin_top(sb, 4, 0);
    w_clock_note = w_wrapped(w_list, UI_MUTED, "");
    lv_obj_t *cap1 = ui_caption(w_list, "FIND A NETWORK");
    lv_obj_set_style_margin_top(cap1, 10, 0);
    w_search_btn = text_btn(w_list, LV_SYMBOL_WIFI " Search for networks", 0, 0, ST_W, UI_ORANGE, w_search_cb);
    w_search_lbl = lv_obj_get_child(w_search_btn, 0);
    lv_obj_set_style_bg_opa(w_search_btn, LV_OPA_50, LV_STATE_DISABLED);
    w_scan_msg = w_wrapped(w_list, UI_MUTED, "");
    w_join_msg = w_wrapped(w_list, UI_MUTED, "");
    w_found = lv_obj_create(w_list);
    lv_obj_remove_style_all(w_found);
    lv_obj_set_size(w_found, ST_W, 0);
    lv_obj_remove_flag(w_found, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#else
    ui_label(w_list, F14, UI_MUTED, "Wi-Fi is compiled out of this firmware.");
#endif
    w_saved_cap = ui_caption(w_list, "SAVED NETWORKS");
    lv_obj_set_style_margin_top(w_saved_cap, 10, 0);
    w_rows = lv_obj_create(w_list);
    lv_obj_remove_style_all(w_rows);
    lv_obj_set_size(w_rows, ST_W, 24);
    lv_obj_remove_flag(w_rows, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    w_note = w_wrapped(w_list, UI_GREEN, "");
    w_wrapped(w_list, UI_MUTED, "Up to 8 networks: join one above, or add it from the desktop app. The board uses them to set its "
                                "clock and, when paired with the app, to send new data while parked.");
    mosaico_board_wifi_list(&w_copy);
    w_gen = w_copy.gen;
    w_clock_gen = w_copy.clock_gen;                      /* an old clock answer is not shown again */
    w_fill_rows();
    ui_set_text(w_note, w_copy.note);
    mosaico_board_request(MB_WIFI_LIST, 0);              /* a fresh copy (the desktop app may have added one) */
    w_asked_ms = esp_timer_get_time() / 1000;
#if CONFIG_OCM_WIFI_CLOCK_SYNC
    w_scan_gen = UINT32_MAX;                             /* the last search list is shown again */
    if (mosaico_board_wifi_scan(&w_scan, w_scan_gen)) w_scan_gen = w_scan.gen;
    w_scan_show();
    mosaico_wifi_join_t j;
    mosaico_board_wifi_join_state(&j);
    w_join_gen = j.gen;                                  /* an old join answer is not shown again */
#endif
}

/* a theme switch re-creates the open app, and the app's destroy closes these pages (the password typed so far, the time
   being set): mosaico_display.c's theme_tick waits until they are closed */
bool ui_apps_overlay_open(void) { return pw_page != NULL || ck_page != NULL; }

static void wifi_destroy(void)
{
    w_list = w_state = w_clock = w_clock_note = w_note = w_rows = NULL;
    w_search_btn = w_search_lbl = w_scan_msg = w_join_msg = w_found = w_saved_cap = NULL;
    memset(w_det, 0, sizeof w_det);
    if (pw_page) lv_async_call(pw_close, NULL);
}

/* a new search list or a changed saved list rebuilds those rows, after the timed ui job (lv_async), never inside it */
static bool w_scan_due, w_rows_due;
static void w_scan_async(void *arg) { (void)arg; w_scan_due = false; if (w_list) w_scan_show(); }
static void w_rows_async(void *arg) { (void)arg; w_rows_due = false; if (w_list) w_fill_rows(); }
static void w_defer(bool *due, lv_async_cb_t fn) { if (!*due) { *due = true; lv_async_call(fn, NULL); } }

static void wifi_update(const ui_snap_t *s)
{
    if (!w_list) return;
    const app_status_t *st = s->st;
    switch (st->wifi) {
    case WIFI_ST_CONNECTED:  ui_set_fmt(w_state, "Connected to %.32s, %d dBm", st->wifi_ssid, st->wifi_rssi); break;
    case WIFI_ST_CONNECTING: ui_set_fmt(w_state, "Connecting to %.32s", st->wifi_ssid); break;
    case WIFI_ST_SCANNING:   ui_set_text(w_state, "Looking for a saved network"); break;
    case WIFI_ST_IDLE:       ui_set_text(w_state, "Idle between clock syncs"); break;
    default:                 ui_set_text(w_state, w_copy.n ? "Off: Sync clock now starts it" : "Off: no network saved yet"); break;
    }
    ui_set_text(w_clock, st->time_synced ? "The clock is synced over Wi-Fi." : "The clock is not synced over Wi-Fi yet.");
#if CONFIG_OCM_WIFI_CLOCK_SYNC
    if (mosaico_board_wifi_scan(&w_scan, w_scan_gen)) { w_scan_gen = w_scan.gen; w_defer(&w_scan_due, w_scan_async); }
    mosaico_wifi_join_t j;
    mosaico_board_wifi_join_state(&j);
    if (j.gen != w_join_gen) { w_join_gen = j.gen; w_join_show(&j); }
#endif
    if (s->now_ms - w_asked_ms >= 5000) { mosaico_board_request(MB_WIFI_LIST, 0); w_asked_ms = s->now_ms; }
    mosaico_wifi_list_t c;
    mosaico_board_wifi_list(&c);
    if (c.gen == w_gen) return;
    w_gen = c.gen;
    bool rows = c.n != w_copy.n;                           /* a network added or forgotten: new rows */
    for (int i = 0; !rows && i < c.n; i++) rows = strcmp(c.net[i].ssid, w_copy.net[i].ssid) != 0;
    bool clock_note = c.clock_gen != w_clock_gen;
    w_clock_gen = c.clock_gen;
    w_copy = c;
    if (rows) w_defer(&w_rows_due, w_rows_async);
    else for (int i = 0; i < c.n; i++) if (w_det[i]) w_detail(i);   /* signal and failures: the label only */
    ui_set_text(w_note, c.note);
    if (clock_note && w_clock_note) {
        ui_set_text(w_clock_note, c.clock_note);
        ui_set_color(w_clock_note, strncmp(c.clock_note, "Looking", 7) ? UI_WARN : UI_MUTED);
    }
}

/* ======================= registry ======================= */
static const ocm_app_t APPS[] = {          /* launcher order and tile colours as on the E_Apps artboard; Settings last */
    { "drive",    "Drive",    LV_SYMBOL_GPS,       drive_create,    drive_update,    drive_destroy,    500, SEC_SKY,    0x8CC8FF, 0x1F6FEB, 0, &ocm_icon_drive_52, NULL, NULL },
    { "ai",       "AI",       LV_SYMBOL_EYE_OPEN,  ai_create,       ai_update,       NULL,             500, SEC_ORANGE, 0xFE860F, 0xE8590C, 0, &ocm_icon_ai_52, NULL, NULL },
    { "learn",    "Learning", LV_SYMBOL_REFRESH,   learn_create,    learn_update,    NULL,             1000, SEC_GREEN, 0x56F487, 0x2B8A3E, 0, &ocm_icon_learn_52, NULL, NULL },
    { "rev",      "R.E.V",    LV_SYMBOL_SHUFFLE,   rev_create,      rev_update,      NULL,             1000, SEC_GREEN, 0x56F487, 0x2B8A3E, 0, &ocm_icon_ai_52, NULL, &ocm_mark_rev_tile },
    { "ev",       "E.V",      LV_SYMBOL_SHUFFLE,   ev_create,       ev_update,       NULL,             1000, SEC_SKY,   0x8CC8FF, 0x1F6FEB, 0, &ocm_icon_ai_52, NULL, &ocm_mark_ev_tile },   /* M1: next to R.E.V */
    { "summary",  "Summary",  LV_SYMBOL_IMAGE,     summary_create,  summary_update,  summary_destroy,  1000, SEC_SKY,   0xFF7AB6, 0xD6336C, 0, &ocm_icon_summary_52, NULL, NULL },
    { "live",     "Live",     LV_SYMBOL_PLAY,      live_create,     live_update,     NULL,             250, SEC_SKY,    0x72ECE4, 0x0C8599, 0, &ocm_icon_live_52, NULL, NULL },
    { "fuel",     "Fuel",     LV_SYMBOL_TINT,      fuel_create,     fuel_update,     NULL,             500, SEC_AMBER,  0xF2A13A, 0xD9480F, 0, &ocm_icon_fuel_52, NULL, NULL },
    { "brakes",   "Brakes",   LV_SYMBOL_STOP,      brakes_create,   brakes_update,   brakes_destroy,   1000, SEC_RED,   0xFD5A4A, 0xE03131, 0, &ocm_icon_brakes_52, NULL, NULL },
    { "trip",     "Trip",     LV_SYMBOL_LOOP,      trip_create,     trip_update,     NULL,             1000, SEC_CYAN,  0x38B8F6, 0x1971C2, 0, &ocm_icon_trip_52, NULL, NULL },
    { "engine",   "Engine",   LV_SYMBOL_WARNING,   engine_create,   engine_update,   NULL,             1000, SEC_AMBER, 0xFCCA21, 0xB8590A, 0, &ocm_icon_engine_52, NULL, NULL },
    { "faults",   "Faults",   LV_SYMBOL_WARNING,   faults_create,   faults_update,   faults_destroy,   1000, SEC_RED,   0xFD5A4A, 0xC92A2A, 0, &ocm_icon_faults_52, NULL, NULL },
    { "board",    "Board",    LV_SYMBOL_SD_CARD,   board_create,    board_update,    NULL,             1000, SEC_AQUA,  0x72ECE4, 0x087F5B, 0, &ocm_icon_board_52, NULL, NULL },
    { "files",    "Files",    LV_SYMBOL_DIRECTORY, files_create,    files_update,    files_destroy,    500, SEC_AQUA,   0xFFB38A, 0x3B5BDB, 0, &ocm_icon_files_52, NULL, NULL },
    { "wifi",     "Wi-Fi",    LV_SYMBOL_WIFI,      wifi_create,     wifi_update,     wifi_destroy,     250,  SEC_CYAN,  0x66D9E8, 0x0B7285, 0, &ocm_icon_wifi_52, NULL, NULL },   /* S1: just before Settings */
    { "settings", "Settings", LV_SYMBOL_SETTINGS,  settings_create, settings_update, settings_destroy, 1000, SEC_VIOLET, 0xB8B8B8, 0x5F6368, 0, &ocm_icon_settings_52, NULL, NULL },   /* keep last */
};

_Static_assert(sizeof APPS / sizeof APPS[0] <= UI_MAX_APPS, "the Home order blob holds UI_MAX_APPS apps");
int ui_app_count(void) { return (int)(sizeof APPS / sizeof APPS[0]); }
const ocm_app_t *ui_app(int i) { return &APPS[i]; }

