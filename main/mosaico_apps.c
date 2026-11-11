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
#include <time.h>
#include "mosaico_files.h"
#include "mosaico_drive.h"
#include "fault_engine.h"
/* mossREV: this build carries CarMind R.E.V only, so every model mark on the screen is the R.E.V mark */
extern const lv_image_dsc_t ocm_mark_rev_34, ocm_mark_rev_24, ocm_mark_rev_tile;
#define LEARNED_STATE "R.E.V"
#define LEARNED_STATE_AND "R.E.V"
extern const lv_image_dsc_t ocm_icon_drive_52, ocm_icon_ai_52, ocm_icon_learn_52, ocm_icon_summary_52, ocm_icon_live_52, ocm_icon_fuel_52, ocm_icon_brakes_52, ocm_icon_trip_52, ocm_icon_engine_52, ocm_icon_faults_52, ocm_icon_board_52, ocm_icon_files_52, ocm_icon_settings_52;
#include "elm327.h"

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

static void d_back_cb(lv_event_t *e) { (void)e; ui_go_home(); }

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

/* header: 30 px round chevron (46 px touch target), title; returns the title label */
/* header: a flex row at the top of the page. Back chevron (38 px round, 46 px touch target), title, items placed after the
   title (d_after), a spacer, items on the right (d_right). No pixel offsets between texts. */
static lv_obj_t *s_hr, *s_hr_sp;
static lv_obj_t *d_header(lv_obj_t *p, const char *title)
{
    s_hr = lv_obj_create(p);
    lv_obj_remove_style_all(s_hr);
    lv_obj_set_pos(s_hr, DX, DHY - 2);
    lv_obj_set_size(s_hr, DW, 38);
    lv_obj_set_flex_flow(s_hr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_hr, 10, 0);
    lv_obj_remove_flag(s_hr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_hr, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *b = lv_obj_create(s_hr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 38, 38);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x141519), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, d_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(b, UI_THEME.f_body, UI_ORANGE, LV_SYMBOL_LEFT));
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
    a_tag = d_after(t, ui_pill(p, "R.E.V", UI_ORANGE));
    a_mark = d_after(t, lv_image_create(p));
    lv_image_set_src(a_mark, &ocm_mark_rev_34);
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
    if (have && m->last_at_ms) ui_set_fmt(a_note, "Last window %lld s ago. The network is frozen; learning runs beside it.",
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
    if (m->learned_ok) { snprintf(b, sizeof b, "%.1f", (double)m->learned_lph); pill_set(a_fu_tag, "LEARNED", UI_GREEN); }
    else if (have) { snprintf(b, sizeof b, "%.1f", (double)m->last_est[2]); pill_set(a_fu_tag, "MODEL", UI_CRIMSON); }
    else snprintf(b, sizeof b, "--");
    d_value_set(&a_fu, b);
    ui_set_color(a_fu.v, m->learned_ok ? UI_GREEN : UI_TEXT);
}

/* ======================= Learning ======================= */
static lv_obj_t *l_saved_dot, *l_saved, *l_f2, *l_f3, *l_k2, *l_k3, *l_tank_u, *l_tbar, *l_tnote, *l_state, *l_n[3];
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
    lv_obj_t *c = d_card(p, DX, DY, hw, 96, UI_BORDER, false);
    d_lab(c, "FUEL READ-OUT", 0, 0, UI_MUTED);
    l_f1 = d_value(c, UI_THEME.f_large, "updates kept", 0, 14);
    l_f2 = d_txt(c, UI_THEME.f_label, UI_GREEN, "", 0, 46);
    l_f3 = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 62);
    c = d_card(p, DX + hw + 8, DY, hw, 96, UI_BORDER, false);
    d_lab(c, "AIRFLOW CONSTANT", 0, 0, UI_MUTED);
    l_k1 = d_value(c, UI_THEME.f_large, "", 0, 14);
    l_k2 = d_txt(c, UI_THEME.f_label, UI_TEXT, "", 0, 46);
    l_k3 = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 62);
    c = d_card(p, DX, DY + 104, DW, 80, UI_BORDER, false);
    d_lab(c, "FUEL TANK", 0, 4, UI_MUTED);
    l_t1 = d_value(c, UI_THEME.f_value, "", 150, 0);
    l_tank_u = l_t1.u;
    l_tbar = d_bar(c, 0, 28, DW - 28, 8, UI_GREEN, 100);
    l_tnote = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 42);
    c = d_card(p, DX, DY + 192, DW, 150, UI_GREEN, false);
    lv_obj_t *mk = lv_image_create(c);                    /* the R.E.V mark next to the model's figures */
    lv_image_set_src(mk, &ocm_mark_rev_24);
    lv_obj_set_pos(mk, 0, -4);
    d_lab(c, "MODEL", 30, 0, UI_GREEN);
    l_state = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    lv_obj_align(l_state, LV_ALIGN_TOP_RIGHT, 0, 0);
    static const char *const N[3] = {"windows read", "self-test", "one run"};
    for (int k = 0; k < 3; k++) {
        l_n[k] = d_txt(c, UI_THEME.f_title, UI_TEXT, "--", k * 140, 26);
        d_txt(c, UI_THEME.f_small, UI_MUTED, N[k], k * 140, 58);
    }
    lv_obj_t *n = d_txt(c, UI_THEME.f_label, UI_MUTED, "The classifier stays as shipped; the fuel read-out learns in this car.", 0, 86);
    lv_obj_set_width(n, DW - 28);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
}

static void learn_update(const ui_snap_t *s)
{
    const rev_status_t *r = &s->rs;
    ui_set_text(l_saved, r->saved ? "Saved on the board" : r->enabled ? "Not saved yet" : "Learning off");
    ui_set_color(l_saved, r->saved ? UI_GREEN : UI_MUTED);
    ui_set_bg(l_saved_dot, r->saved ? UI_GREEN : UI_THEME.dim);
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
    if (m->ready) ui_set_fmt(l_state, "R.E.V %s", ml_model_version(ML_MODEL_REV)); else ui_set_text(l_state, "R.E.V not loaded");
    ui_set_fmt(l_n[0], "%lu", (unsigned long)m->windows);
    ui_set_text(l_n[1], m->ready ? selftest_word() : "--");
    if (m->ready && m->selftest_us) ui_set_fmt(l_n[2], "%.1f ms", m->selftest_us / 1000.0); else ui_set_text(l_n[2], "--");
}

/* ======================= R.E.V: the model in use, its self-test and the fuel learning ======================= */
static lv_obj_t *v_step, *v_card[2], *v_big[2], *v_l1[2], *v_l2[2], *v_learn, *v_z, *v_n[3], *v_saved, *v_resets;

static void rev_create(lv_obj_t *p)
{
    lv_obj_t *t = d_header(p, "R.E.V");
    lv_obj_t *mk = d_after(t, lv_image_create(p));
    lv_image_set_src(mk, &ocm_mark_rev_34);
    d_after(mk, ui_label(p, UI_THEME.f_label, UI_MUTED, "the model in use"));
    v_step = d_right(p, "", UI_MUTED);
    int hw = (DW - 8) / 2;
    static const char *const R[2] = {"MODEL", "SELF-TEST"};
    for (int k = 0; k < 2; k++) {
        lv_obj_t *c = v_card[k] = d_card(p, DX + k * (hw + 8), DY, hw, 120, UI_THEME.dim, true);
        lv_obj_set_style_radius(c, 20, 0);
        d_lab(c, R[k], 0, 0, UI_MUTED);
        v_big[k] = d_txt(c, UI_THEME.f_title, UI_TEXT, "--", 0, 18);
        v_l1[k] = d_txt(c, UI_THEME.f_label, UI_TEXT, "", 0, 54);
        v_l2[k] = d_txt(c, UI_THEME.f_label, UI_MUTED, "", 0, 74);
    }
    lv_obj_t *c = d_card(p, DX, DY + 128, DW, 46, UI_BORDER, false);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_ver(c, 0, 0);
    lv_obj_t *ok = ui_label(c, UI_THEME.f_value, UI_GREEN, LV_SYMBOL_OK);
    lv_obj_align(ok, LV_ALIGN_LEFT_MID, 0, 0);
    v_learn = ui_label(c, UI_THEME.f_body, UI_TEXT, "");
    lv_obj_align(v_learn, LV_ALIGN_LEFT_MID, 30, 0);
    v_z = ui_label(c, UI_THEME.f_label, UI_MUTED, "");
    lv_obj_align(v_z, LV_ALIGN_RIGHT_MID, 0, 0);
    c = d_card(p, DX, DY + 182, DW, 160, UI_BORDER, false);
    d_lab(c, "FUEL LEARNING IN THIS CAR", 0, 0, UI_GREEN);
    static const char *const N[3] = {"updates taken", "kept as is", "pairs seen"};   /* rev_learn.c: commits, kept, pairs */
    for (int k = 0; k < 3; k++) {
        v_n[k] = d_txt(c, UI_THEME.f_large, UI_TEXT, "--", k * 140, 28);
        d_txt(c, UI_THEME.f_small, UI_MUTED, N[k], k * 140, 64);
    }
    v_saved = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 116);
    v_resets = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
    lv_obj_align(v_resets, LV_ALIGN_TOP_RIGHT, 0, 116);
}

static void rev_update(const ui_snap_t *s)
{
    const ml_stats_t *m = &s->ml;
    const rev_status_t *r = &s->rs;
    if (m->windows) ui_set_fmt(v_step, "%lu windows, %.1f ms", (unsigned long)m->windows, m->last_us / 1000.0);
    else ui_set_text(v_step, m->ready ? "no windows yet" : "not loaded");
    int t = m->ready ? ml_selftest_state() : -1;
    uint32_t edge[2] = {m->ready ? UI_GREEN : UI_THEME.dim, t > 0 ? UI_GREEN : t == 0 ? UI_WARN : UI_THEME.dim};
    for (int k = 0; k < 2; k++)
        if (lv_color_to_u32(lv_obj_get_style_border_color(v_card[k], 0)) != lv_color_to_u32(lv_color_hex(edge[k])))
            lv_obj_set_style_border_color(v_card[k], lv_color_hex(edge[k]), 0);
    ui_set_text(v_big[0], m->ready ? "CarMind R.E.V" : "--");
    ui_set_fmt(v_l1[0], "model %s", ml_model_version(ML_MODEL_REV));
    ui_set_fmt(v_l2[0], "for %s, %s", ml_model_target(ML_MODEL_REV), m->ready ? "loaded" : "not loaded");
    ui_set_text(v_big[1], m->ready ? selftest_word() : "--");
    if (m->ready && m->selftest_us) ui_set_fmt(v_l1[1], "one run %.1f ms", m->selftest_us / 1000.0); else ui_set_text(v_l1[1], "");
    ui_set_text(v_l2[1], t >= 0 ? "vs the PC simulation, 1 LSB" : "");
    ui_set_text(v_learn, r->learning ? "Learning fuel use in this car" : r->safe ? "Learning off: safe mode" : "Learning off");
    ui_set_text(v_z, m->z_ok ? "z output yes" : "z output no");
    ui_set_fmt(v_n[0], "%lu", (unsigned long)r->commits);
    ui_set_fmt(v_n[1], "%lu", (unsigned long)r->kept);
    ui_set_fmt(v_n[2], "%lu", (unsigned long)r->pairs);
    ui_set_text(v_saved, r->saved ? "Saved on the board" : r->enabled ? "Not saved yet" : "Learning off");
    ui_set_fmt(v_resets, "%lu reset%s to factory", (unsigned long)r->resets, r->resets == 1 ? "" : "s");
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
        ui_set_text(f_verdict, m->learned_ok ? "comparing: needs measured fuel labels" : "learner waits for measured fuel labels");
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
    ui_set_text(br_note[1], tq ? "5 s mean, from fuel and rpm" : "model estimate (no physics torque yet)");
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

/* ======================= Drive (H_Drive): pure black, dark cards, no light tile. Flex rows and columns throughout ===== */
#define RPM_BLOCKS 20
#define TANK_BLOCKS 10
static const uint32_t DCLS[4] = {0xAD8DFD, 0xFD5A4A, 0x5AA2FF, 0xFCCA21};     /* stopped, braking, accelerating, cruising */
static lv_obj_t *d_ctl, *d_spd, *d_rpm, *d_rpm_blk[RPM_BLOCKS], *d_ai, *d_face[4], *d_cls, *d_sure, *d_pb[4], *d_mark,
                *d_ev[3], *d_tag[3], *d_tank_blk[TANK_BLOCKS], *d_tank_pct, *d_tank_km;
static int d_rpm_lit = -1, d_tank_lit = -1;

static lv_obj_t *dk_card(lv_obj_t *p, int radius)
{
    lv_obj_t *c = fbox(p, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, 4);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x111216), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1F2026), 0);
    lv_obj_set_style_radius(c, radius, 0);
    lv_obj_set_style_pad_hor(c, 14, 0);
    lv_obj_set_style_pad_ver(c, 10, 0);
    return c;
}
static lv_obj_t *dk_lab(lv_obj_t *p, const char *t)
{
    lv_obj_t *l = ui_label(p, UI_THEME.f_small, 0x8E8E93, t);
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
    for (int i = 0; i < n; i++) { blk[i] = ui_rect(r, 0, 0, 1, h, 0x1F2026, 2); lv_obj_set_flex_grow(blk[i], 1); }
    return r;
}

static void drive_create(lv_obj_t *p)
{
    lv_obj_t *col = fbox(p, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_pos(col, 22, 54);
    lv_obj_set_size(col, UI_W - 44, UI_H - 16 - 54);
    /* header: back, Drive, End drive pill */
    lv_obj_t *hr = fbox(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 10);
    lv_obj_set_size(hr, LV_PCT(100), 38);
    lv_obj_t *b = lv_obj_create(hr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 38, 38);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x141519), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, d_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(b, UI_THEME.f_body, 0x8CC8FF, LV_SYMBOL_LEFT));
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
    d_spd = ui_label(vr, UI_THEME.f_hero, 0x8CC8FF, "--");
    lv_obj_t *u = ui_label(vr, UI_THEME.f_label, 0x8E8E93, "km/h");
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
    lv_obj_set_style_bg_color(d_ai, lv_color_hex(0x111216), 0);
    lv_obj_set_style_bg_opa(d_ai, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d_ai, 4, 0);
    lv_obj_set_style_border_side(d_ai, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(d_ai, lv_color_hex(DCLS[3]), 0);
    lv_obj_set_style_radius(d_ai, 20, 0);
    lv_obj_set_style_pad_hor(d_ai, 12, 0);
    lv_obj_t *fb = ui_rect(d_ai, 0, 0, 56, 56, 0x1A1B20, 14);
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
        lv_obj_set_style_bg_color(d_pb[k], lv_color_hex(0x26272D), 0);
        lv_obj_set_style_bg_opa(d_pb[k], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(d_pb[k], 3, 0);
        lv_obj_set_style_radius(d_pb[k], 0, LV_PART_INDICATOR);
        lv_obj_remove_flag(d_pb[k], LV_OBJ_FLAG_CLICKABLE);
        ui_label(pc, UI_THEME.f_small, 0x8E8E93, K[k]);
    }
    d_mark = lv_image_create(d_ai);
    lv_image_set_src(d_mark, &ocm_mark_rev_34);
    /* three estimate tiles: label + outlined tag, value + unit */
    r = fbox(col, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_set_size(r, LV_PCT(100), 78);
    static const char *const EL[3] = {"BRAKE", "TORQUE", "FUEL"}, *const ET[3] = {"MODEL", "MODEL", "LEARNED"};
    /* fixed widths, 420 px with the two 8 px gaps. With 1 px letter spacing and 4 px pill padding the label row needs
       128 px for "TORQUE" + "PHYSICS", 110 for "BRAKE" + "MODEL" and 112 for "FUEL" + "LEARNED" (Montserrat 12 advance
       widths); each tile leaves width - 2 x 8 padding - 2 x 1 border for it */
    static const int EW[3] = {134, 152, 134};
    const uint32_t EC[3] = {0xFD5A4A, 0xF2A13A, 0x56F487}, TC[3] = {0xF2A13A, 0xF2A13A, 0x56F487};
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
    lv_obj_set_style_bg_color(c, lv_color_hex(0x111216), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1F2026), 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_hor(c, 14, 0);
    dk_lab(c, "TANK");
    lv_obj_t *tb = dk_blocks(c, d_tank_blk, TANK_BLOCKS, 12);
    lv_obj_set_width(tb, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tb, 1);
    d_tank_pct = ui_label(c, UI_THEME.f_label, UI_TEXT, "--");
    d_tank_km = ui_label(c, UI_THEME.f_small, 0x8E8E93, "");
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
        for (int i = 0; i < RPM_BLOCKS; i++) ui_set_bg(d_rpm_blk[i], i >= lit ? 0x1F2026 : i >= RPM_BLOCKS - 4 ? 0xFD122E : 0x8CC8FF);
    }
    bool have = m->last_class >= 0;
    uint32_t cc = have ? DCLS[m->last_class] : 0x4A4A4A;
    if (lv_color_to_u32(lv_obj_get_style_border_color(d_ai, 0)) != lv_color_to_u32(lv_color_hex(cc))) lv_obj_set_style_border_color(d_ai, lv_color_hex(cc), 0);
    if (lv_color_to_u32(lv_obj_get_style_border_color(d_face[0], 0)) != lv_color_to_u32(lv_color_hex(cc))) lv_obj_set_style_border_color(d_face[0], lv_color_hex(cc), 0);
    for (int k = 1; k < 4; k++) ui_set_bg(d_face[k], cc);
    ui_set_text(d_cls, have ? ui_class_name(m->last_class) : "--");
    ui_set_color(d_cls, cc);
    if (have) ui_set_fmt(d_sure, "%d %% sure", pct(m->last_prob)); else ui_set_text(d_sure, m->ready ? "waits for a drive" : "model not loaded");
    for (int k = 0; k < 4; k++) {
        bar_set(d_pb[k], s->nprobs > k ? pct(s->probs[k]) : 0);
        lv_obj_set_style_bg_color(d_pb[k], lv_color_hex(have && k == m->last_class ? DCLS[k] : 0x4A4A4A), LV_PART_INDICATOR);
    }
    if (have) ui_set_fmt(d_ev[0], "%.1f bar", (double)m->last_est[0]); else ui_set_text(d_ev[0], "--");
    ui_set_text(d_tag[1], TORQUE_SRC(s));
    if (torque_physics(s)) ui_set_fmt(d_ev[1], "%.0f N m", (double)o->torque_nm);
    else if (have) ui_set_fmt(d_ev[1], "%.0f N m", (double)m->last_est[1]);
    else ui_set_text(d_ev[1], "--");
    if (m->learned_ok) { ui_set_fmt(d_ev[2], "%.1f L/h", (double)m->learned_lph); ui_set_text(d_tag[2], "LEARNED"); }
    else if (s->obd_live && o->fuel_lph_calc >= 0) {
        ui_set_fmt(d_ev[2], "%.1f L/h", (double)o->fuel_lph_calc);
        char up[12]; const char *src = fuel_src_name((fuel_src_t)o->fuel_src); int i = 0;
        for (; src[i] && i < 11; i++) up[i] = (char)toupper((unsigned char)src[i]);
        up[i] = 0;
        ui_set_text(d_tag[2], up);
    } else ui_set_text(d_ev[2], "--");
    const rev_tank_out_t *t = &s->rs.tank;
    float level = t->ready ? t->level : have_f(s, F_fuel_level) ? fv(s, F_fuel_level) : -1;
    int tl = level >= 0 ? (int)lroundf(fminf(level, 100) / 10.0f) : 0;
    if (tl != d_tank_lit) { d_tank_lit = tl; for (int i = 0; i < TANK_BLOCKS; i++) ui_set_bg(d_tank_blk[i], i < tl ? 0x56F487 : 0x1F2026); }
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
    return sev >= SEV_CRITICAL ? 0xFD122E : sev >= SEV_HIGH ? 0xFE860F : sev >= SEV_MEDIUM ? 0xFCCA21 : 0x8CC8FF;
}

static void faults_create(lv_obj_t *p)
{
    lv_obj_t *t = d_header(p, "Faults");
    q_mil = d_after(t, dk_tag(p, "MIL ON", 0xFD122E));
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
        if (f->is_new) dk_tag(hr, "NEW", 0x56F487);
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
    if (s->st->fault_rev != q_rev) { q_rev = s->st->fault_rev; q_read_ms = s->now_ms; faults_fill(s); }
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
        const char *K[4] = {"DISTANCE", "TIME", "ECONOMY", "HARD BRAKES"};
        char V[4][16], U[4][12];
        snprintf(V[0], 16, "%.1f", (double)sm.km); snprintf(U[0], 12, "km");
        snprintf(V[1], 16, "%ld", (long)(sm.dur_s / 60)); snprintf(U[1], 12, "min");
        snprintf(V[2], 16, "%s", e); snprintf(U[2], 12, "%s", strcmp(eu, "L/100 km") ? eu : "L/100");
        snprintf(V[3], 16, "%d", sm.hard_brakes); U[3][0] = 0;
        for (int k = 0; k < 4; k++) {
            lv_obj_t *c = d_card(sm_p1, DX + k * (w4 + 8), DY, w4, 58, UI_BORDER, false);
            lv_obj_set_style_radius(c, 14, 0);
            lv_obj_set_style_pad_all(c, 8, 0);
            d_lab(c, K[k], 0, 0, UI_MUTED);
            d_val_t v = d_value(c, UI_THEME.f_value, U[k], 0, 18);
            d_value_set(&v, V[k]);
        }
        /* speed and fuel rate over the drive, one chart, fuel on the secondary axis */
        lv_obj_t *c = d_card(sm_p1, DX, DY + 66, DW, 150, UI_BORDER, false);
        lv_obj_set_style_pad_ver(c, 8, 0);
        d_lab(c, "SPEED KM/H", 0, 0, UI_THEME.section[SEC_SKY]);
        lv_obj_t *fl = d_lab(c, "FUEL L/H", 0, 0, UI_GREEN);
        lv_obj_align(fl, LV_ALIGN_TOP_RIGHT, 0, 0);
        float vmax = 10, fmax = 1;
        for (int i = 0; i < sm.n; i++) { vmax = fmaxf(vmax, sm.spd[i]); fmax = fmaxf(fmax, sm.fuel[i]); }
        lv_obj_t *ch = sm_chart(c, 0, 18, DW - 28, 96, LV_CHART_TYPE_LINE, sm.n, (int)vmax + 5);
        lv_chart_set_axis_range(ch, LV_CHART_AXIS_SECONDARY_Y, 0, (int)(fmax * 10) + 5);
        lv_chart_series_t *ss = lv_chart_add_series(ch, lv_color_hex(UI_THEME.section[SEC_SKY]), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_series_t *sf = lv_chart_add_series(ch, lv_color_hex(UI_GREEN), LV_CHART_AXIS_SECONDARY_Y);
        for (int i = 0; i < sm.n; i++) {
            lv_chart_set_next_value(ch, ss, (int32_t)lroundf(sm.spd[i]));
            lv_chart_set_next_value(ch, sf, (int32_t)lroundf(sm.fuel[i] * 10));
        }
        d_txt(c, UI_THEME.f_small, UI_MUTED, "0 min", 0, 118);
        lv_obj_t *mid = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
        ui_set_fmt(mid, "%ld", (long)(sm.dur_s / 120));
        lv_obj_align(mid, LV_ALIGN_TOP_MID, 0, 118);
        lv_obj_t *end = ui_label(c, UI_THEME.f_small, UI_MUTED, "");
        ui_set_fmt(end, "%ld min", (long)(sm.dur_s / 60));
        lv_obj_align(end, LV_ALIGN_TOP_RIGHT, 0, 118);
        /* what the AI saw, one block per 10 s (at most 40 blocks), and the class shares */
        c = d_card(sm_p1, DX, DY + 224, DW, 104, UI_BORDER, false);
        lv_obj_set_style_pad_ver(c, 8, 0);
        d_lab(c, "WHAT THE AI SAW, EVERY 10 S", 0, 0, UI_MUTED);
        int nb = sm.dur_s / 10;
        if (nb > 40) nb = 40;
        if (nb < 1) nb = 1;
        int W = DW - 28, bw = W / nb;
        for (int k = 0; k < nb && sm.n > 0; k++) {
            int cls = sm.cls[(int)((long)k * sm.n / nb)];
            ui_rect(c, k * bw, 22, bw > 1 ? bw - 1 : 1, 16, cls >= 0 && cls < 4 ? UI_THEME.cls[cls] : UI_THEME.track, 0);
        }
        static const char *const CN[4] = {"Stopped", "Braking", "Accelerating", "Cruising"};
        int order[4] = {0, 1, 2, 3};
        for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) if (sm.cls_share[order[j]] > sm.cls_share[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        int x = 0;
        for (int i = 0; i < 4; i++) {
            int k = order[i];
            ui_rect(c, x, 54, 9, 9, UI_THEME.cls[k], 2);
            lv_obj_t *l = d_txt(c, UI_THEME.f_small, UI_TEXT, "", x + 14, 50);
            ui_set_fmt(l, "%s %.0f %%", CN[k], sm.cls_share[k] * 100.0);
            lv_obj_update_layout(l);
            x += 14 + lv_obj_get_width(l) + 12;
        }
        lv_obj_t *a = d_txt(c, UI_THEME.f_small, UI_MUTED, "", 0, 70);
        ui_set_fmt(a, "%d brakings", sm.brake_events);
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

static void summary_destroy(void) { sm_tv = sm_p1 = sm_p2 = sm_hdr = NULL; }

static void summary_update(const ui_snap_t *s)
{
    (void)s;
    mosaico_drive_get_summary(&sm);
    mosaico_drive_get_history(&sm_h);
    if (sm.gen == sm_gen && sm_h.gen == sm_hgen) return;
    sm_gen = sm.gen; sm_hgen = sm_h.gen;
    /* sm_fill cleans the pages: the dots go with them, so draw them again */
    sm_fill();
    for (int t = 0; t < 2; t++) {
        lv_obj_t *pg = t ? sm_p2 : sm_p1;
        int x = UI_W / 2 - 15;
        ui_rect(pg, x, 456, t ? 6 : 18, 6, t ? UI_THEME.dim : UI_TEXT, LV_RADIUS_CIRCLE);
        ui_rect(pg, x + (t ? 13 : 25), 456, t ? 18 : 6, 6, t ? UI_TEXT : UI_THEME.dim, LV_RADIUS_CIRCLE);
    }
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

static void fl_row_cb(lv_event_t *e) { lv_async_call(fl_show_detail, lv_event_get_user_data(e)); }
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
    ui_confirm("Delete ALL drive logs on the board? This cannot be undone.", "Continue", fl_ask_all_again, lv_event_get_user_data(e));
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
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    snprintf(q, sizeof q, "Delete drive log s%06lu and all its files? This cannot be undone.", (unsigned long)id);
    ui_confirm(q, "Delete", fl_do_delete, (void *)(uintptr_t)id);
}

static void fl_fill_list(void)
{
    lv_obj_clean(fl_body);
    const mf_list_t *L = &fl_list;
    /* storage: used bar and free of total */
    lv_obj_t *sc = ui_card(fl_body, 0, 0, CW - 16, 70, UI_BORDER);
    ui_caption(sc, "STORAGE");
    lv_obj_t *sl = ui_label(sc, F14, UI_TEXT, "--");
    lv_obj_align(sl, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_t *sb = bar(sc, 0, 26, CW - 16 - 2 * UI_THEME.pad_card, 10, UI_ORANGE, 1000);
    if (L->total_kb) {
        char a[16], b[16];
        fmt_size(a, sizeof a, L->free_kb * 1024u);
        fmt_size(b, sizeof b, L->total_kb * 1024u);
        ui_set_fmt(sl, "%s free of %s", a, b);
        bar_set(sb, (int)((uint64_t)(L->total_kb - L->free_kb) * 1000 / L->total_kb));
        if (L->free_kb * 10 < L->total_kb) lv_obj_set_style_bg_color(sb, lv_color_hex(UI_RED), LV_PART_INDICATOR);
    }
    if (!L->ok) { ui_label(fl_body, F16, UI_MUTED, L->busy || !L->gen ? "Reading the log storage..." : "No log storage"); return; }
    if (L->note[0]) ui_label(fl_body, F14, UI_GREEN, L->note);
    if (!L->n) ui_label(fl_body, F16, UI_MUTED, "No drive logs yet.");
    int deletable = 0;
    for (int i = 0; i < L->n; i++) {
        const mf_session_t *s = &L->s[i];
        deletable += !s->current;
        lv_obj_t *r = lv_obj_create(fl_body);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, CW - 16, 72);
        lv_obj_set_style_bg_color(r, lv_color_hex(UI_CARD), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, UI_THEME.radius_row, 0);
        lv_obj_set_style_border_width(r, UI_THEME.border_card, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(UI_BORDER), 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(r, fl_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)s->id);
        char when[24], size[16], car[16] = "";
        fmt_when(when, sizeof when, s->start_epoch_ms);
        fmt_size(size, sizeof size, s->bytes);
        if (s->model_year > 1980) snprintf(car, sizeof car, "  MY %d", s->model_year);
        lv_obj_t *t = ui_label(r, F20, UI_TEXT, "");
        ui_set_fmt(t, "s%06lu", (unsigned long)s->id);
        lv_obj_set_pos(t, 14, 10);
        lv_obj_t *z = ui_label(r, F14, UI_MUTED, size);
        lv_obj_set_pos(z, 130, 14);
        lv_obj_t *d = ui_label(r, F14, UI_MUTED, "");
        ui_set_fmt(d, "%s  %lu rows%s", when, (unsigned long)s->rows, car);
        lv_obj_set_pos(d, 14, 42);
        if (s->current) {
            lv_obj_t *w = ui_pill(r, "WRITING", UI_GREEN);
            lv_obj_align(w, LV_ALIGN_RIGHT_MID, -12, 0);
        } else {
            lv_obj_t *b = lv_obj_create(r);                  /* 44 px trash button: opens the confirm sheet, never deletes */
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, 44, 44);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -12, 0);
            lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(b, fl_trash_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)s->id);
            lv_obj_center(ui_label(b, F16, UI_RED, LV_SYMBOL_TRASH));
        }
    }
    if (L->total > L->n) ui_set_fmt(ui_label(fl_body, F14, UI_MUTED, ""), "newest %d of %d shown", L->n, L->total);
    if (deletable) wide_button(fl_body, LV_SYMBOL_TRASH "  Delete all drive logs", UI_RED, fl_all_cb, (void *)(intptr_t)deletable);
    lv_obj_t *n = ui_label(fl_body, F14, UI_MUTED, "Learned " LEARNED_STATE_AND " state, the clock and config.json are never deleted here.");
    lv_obj_set_width(n, CW - 24);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
}

static void fl_fill_detail(void)
{
    lv_obj_clean(fl_body);
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

static void fl_show_list(void *arg) { (void)arg; if (!fl_body) return; fl_mode = 0; fl_gen = 0; mosaico_board_request(MB_FILES_SCAN, 0); fl_fill_list(); }
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

static void files_destroy(void) { fl_body = NULL; }

static void files_update(const ui_snap_t *s)
{
    (void)s;
    if (fl_mode == 0) {
        mf_list_t *L = &fl_list;
        mosaico_files_get_list(L);
        if (L->gen != fl_gen && !L->busy) { fl_gen = L->gen; fl_fill_list(); }
    } else {
        mosaico_files_get_detail(&fl_det);
        if (fl_det.gen != fl_dgen && fl_det.id == fl_id) { fl_dgen = fl_det.gen; fl_fill_detail(); }
    }
}

/* ======================= 12. Settings ======================= */
static lv_obj_t *g_list, *g_ble, *g_wifi;
static void settings_fill(void);
static void rebuild_async(void *arg) { (void)arg; settings_fill(); }

static void chip_cb(lv_event_t *e)
{
    int app = (int)(intptr_t)lv_event_get_user_data(e);
    ui_settings_t s;
    ui_settings_get(&s);
    s.hidden ^= (uint16_t)(1u << app);
    ui_settings_set(&s);
    lv_async_call(rebuild_async, NULL);
}

static void move_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);     /* pos * 2 + (0 up, 1 down) */
    int pos = code / 2, dir = code % 2 ? 1 : -1;
    ui_settings_t s;
    ui_settings_get(&s);
    int to = pos + dir;
    if (to < 0 || to >= s.n) return;
    uint8_t t = s.order[pos]; s.order[pos] = s.order[to]; s.order[to] = t;
    ui_settings_set(&s);
    lv_async_call(rebuild_async, NULL);                       /* not from inside the clicked button's own event */
}

static void widget_cb(lv_event_t *e)
{
    ui_settings_t s;
    ui_settings_get(&s);
    s.widget = (uint8_t)lv_dropdown_get_selected(lv_event_get_target_obj(e));
    ui_settings_set(&s);
}

/* segmented controls: user data = (setting << 8) | option */
enum { SEG_THEME = 1, SEG_UNITS, SEG_OFF };
static const uint16_t OFF_S[] = {0, 10, 20, 60};
static void seg_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e), what = code >> 8, opt = code & 0xFF;
    ui_settings_t s;
    ui_settings_get(&s);
    if (what == SEG_THEME) s.theme = (uint8_t)opt;
    else if (what == SEG_UNITS) s.econ_l100 = (uint8_t)opt;
    else if (what == SEG_OFF) s.screen_off_s = OFF_S[opt];
    ui_settings_set(&s);
    lv_async_call(rebuild_async, NULL);
}

static lv_obj_t *g_bright_bar, *g_bright_val;
static void bright_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    ui_settings_t s;
    ui_settings_get(&s);
    int v = s.brightness + d;
    s.brightness = (uint8_t)(v < 10 ? 10 : v > 100 ? 100 : v);
    ui_settings_set(&s);
    if (g_bright_bar) bar_set(g_bright_bar, s.brightness);
    if (g_bright_val) ui_set_fmt(g_bright_val, "%d %%", s.brightness);
}

static lv_obj_t *g_store;
static void do_format(void *arg) { (void)arg; mosaico_board_request(MB_FORMAT, 0); }
static void format_again(void *arg)
{
    (void)arg;
    ui_confirm("Format now? Drive logs and summaries cannot be recovered. Learned state and settings are kept.", "Format now", do_format, NULL);
}
static void format_cb(lv_event_t *e)
{
    (void)e;
    if (elm_drive_active()) { ui_confirm("A drive is active. End the drive first, then format.", "OK", NULL, NULL); return; }
    ui_confirm("Erase all drive logs and summaries on this board?", "Continue", format_again, NULL);
}

static void do_forget(void *arg) { (void)arg; mosaico_board_request(MB_BLE_FORGET, 0); }
static void forget_cb(lv_event_t *e) { (void)e; ui_confirm("Forget the paired OBD adapter? The board then searches for one again.", "Forget", do_forget, NULL); }
static void search_cb(lv_event_t *e) { (void)e; mosaico_board_request(MB_BLE_SEARCH, 0); }
#if CONFIG_OCM_WIFI_CLOCK_SYNC
static void wifi_cb(lv_event_t *e) { (void)e; mosaico_board_request(MB_WIFI_NOW, 0); }
#endif

static lv_obj_t *small_btn(lv_obj_t *p, const char *sym, int x, int y, lv_event_cb_t cb, int code)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, 44, 40);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_radius(b, UI_THEME.radius_button, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    lv_obj_center(ui_label(b, F16, UI_TEXT, sym));
    return b;
}

static lv_obj_t *text_btn(lv_obj_t *p, const char *t, int x, int y, int w, uint32_t c, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 46);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_THEME.track), 0);
    lv_obj_set_style_border_width(b, UI_THEME.border_card, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(c), 0);
    lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(b, F16, c, t));
    return b;
}

/* a row of n equal segments; the selected one is filled with the app accent */
static void segmented(lv_obj_t *p, int y, const char *const *opts, int n, int sel, int what)
{
    int w = (380 - (n - 1) * 6) / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, 4 + i * (w + 6), y);
        lv_obj_set_size(b, w, 44);
        lv_obj_set_style_radius(b, UI_THEME.radius_row, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i == sel ? UI_ORANGE : UI_THEME.track), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)((what << 8) | i));
        lv_obj_center(ui_label(b, F16, i == sel ? UI_BG : UI_TEXT, opts[i]));
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

static void settings_fill(void)
{
    if (!g_list) return;
    lv_obj_clean(g_list);
    g_bright_bar = g_bright_val = NULL;
    ui_settings_t s;
    ui_settings_get(&s);
    int y = 0;
    static const char *const TH[] = {"Auto", "Light", "Dark"};
    at(ui_caption(g_list, "THEME (HOME AND APPS)"), 4, y); y += 22;
    segmented(g_list, y, TH, 3, s.theme, SEG_THEME); y += 54;
    at(ui_label(g_list, F14, UI_MUTED, "Auto: Light 06:00 to 17:00 once the clock is set, else Dark."), 4, y); y += 30;
    at(ui_caption(g_list, "HOME APPS (TAP TO SHOW / HIDE) AND ORDER"), 4, y); y += 24;
    for (int i = 0; i < s.n; i++) {
        int app = s.order[i];
        bool on = !(s.hidden & (1u << app)), settings = app == ui_app_count() - 1;
        lv_obj_t *c = lv_obj_create(g_list);
        lv_obj_remove_style_all(c);
        lv_obj_set_pos(c, 4, y);
        lv_obj_set_size(c, 270, 40);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(on ? UI_THEME.section[ui_app(app)->section] : UI_BORDER), 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(on ? UI_CARD : UI_BG), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        if (!settings) { lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(c, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app); }
        lv_obj_t *l = ui_label(c, F16, on ? UI_TEXT : UI_MUTED, "");
        ui_set_fmt(l, "%s  %s%s", ui_app(app)->glyph, ui_app(app)->name, settings ? " (always)" : on ? "" : "   hidden");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 14, 0);
        small_btn(g_list, LV_SYMBOL_UP, 284, y, move_cb, i * 2);
        small_btn(g_list, LV_SYMBOL_DOWN, 334, y, move_cb, i * 2 + 1);
        y += 46;
    }
    y += 8;
    at(ui_caption(g_list, "VALUE UNDER THE SPEED"), 4, y); y += 22;
    lv_obj_t *d = lv_dropdown_create(g_list);
    lv_obj_set_pos(d, 4, y);
    lv_obj_set_width(d, 380);
    lv_dropdown_set_options(d, "Fuel rate (L/h)\nFuel use now (L/100 km)\nEconomy (units below)\nSpeed\nEngine speed\nTorque");
    lv_dropdown_set_selected(d, s.widget);
    style_dropdown(d);
    lv_obj_add_event_cb(d, widget_cb, LV_EVENT_VALUE_CHANGED, NULL);
    y += 56;
    static const char *const UN[] = {"km/L", "L/100 km"};
    at(ui_caption(g_list, "ECONOMY UNITS"), 4, y); y += 22;
    segmented(g_list, y, UN, 2, s.econ_l100 ? 1 : 0, SEG_UNITS); y += 58;
    at(ui_caption(g_list, "BRIGHTNESS"), 4, y); y += 22;
    small_btn(g_list, LV_SYMBOL_MINUS, 4, y, bright_cb, -10);
    g_bright_bar = bar(g_list, 58, y + 15, 230, 10, UI_ORANGE, 100);
    bar_set(g_bright_bar, s.brightness);
    small_btn(g_list, LV_SYMBOL_PLUS, 296, y, bright_cb, 10);
    g_bright_val = at(ui_label(g_list, F14, UI_MUTED, ""), 346, y + 10);
    ui_set_fmt(g_bright_val, "%d %%", s.brightness);
    y += 54;
    static const char *const OFF[] = {"Never", "10 s", "20 s", "60 s"};
    int sel = 0;
    for (int i = 0; i < 4; i++) if (OFF_S[i] == s.screen_off_s) sel = i;
    at(ui_caption(g_list, "SCREEN OFF AFTER"), 4, y); y += 22;
    segmented(g_list, y, OFF, 4, sel, SEG_OFF); y += 58;
    at(ui_caption(g_list, "OBD ADAPTER (BLUETOOTH)"), 4, y); y += 22;
    g_ble = at(ui_label(g_list, F16, UI_TEXT, "--"), 4, y); y += 28;
    text_btn(g_list, LV_SYMBOL_REFRESH " Search now", 4, y, 186, UI_ORANGE, search_cb);
    text_btn(g_list, LV_SYMBOL_CLOSE " Forget", 198, y, 186, UI_RED, forget_cb);
    y += 60;
    at(ui_caption(g_list, "WI-FI CLOCK"), 4, y); y += 22;
    g_wifi = at(ui_label(g_list, F16, UI_TEXT, "--"), 4, y); y += 28;
#if CONFIG_OCM_WIFI_CLOCK_SYNC
    text_btn(g_list, LV_SYMBOL_WIFI " Sync clock now", 4, y, 380, UI_ORANGE, wifi_cb);
    y += 60;
    at(ui_label(g_list, F14, UI_MUTED, "Networks are added from the desktop app."), 4, y); y += 30;
#else
    at(ui_label(g_list, F14, UI_MUTED, "Wi-Fi is compiled out of this firmware."), 4, y); y += 30;
#endif
    at(ui_caption(g_list, "STORAGE"), 4, y); y += 22;
    g_store = at(ui_label(g_list, F16, UI_TEXT, "--"), 4, y); y += 28;
    text_btn(g_list, LV_SYMBOL_WARNING " Format storage", 4, y, 380, UI_RED, format_cb);
    y += 56;
    lv_obj_t *fn = at(ui_label(g_list, F14, UI_MUTED, "Erases drive logs, summaries and drives.csv. Learned " LEARNED_STATE " state, "
                                                      "clock and config files are copied to RAM and written back."), 4, y);
    lv_obj_set_width(fn, 380);
    lv_label_set_long_mode(fn, LV_LABEL_LONG_WRAP);
    y += 64;
    at(ui_caption(g_list, "FIRMWARE AND BOARD"), 4, y); y += 22;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    lv_obj_t *info = at(ui_label(g_list, F14, UI_TEXT, ""), 4, y);
    ui_set_fmt(info, "%s\nESP-IDF %s\nESP32-S31 rev %d.%d, %d cores\nFlash %lu MB, PSRAM %u MB\nESP-Mosaico V1.2", CARMIND_FW_STRING,
               esp_get_idf_version(), ci.revision / 100, ci.revision % 100, ci.cores, (unsigned long)(flash >> 20),
               (unsigned)(esp_psram_get_size() >> 20));
    y += 104;
    at(ui_label(g_list, F14, UI_MUTED, "Saved on the board (NVS). Function key: home."), 4, y);
}

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
}

static void settings_destroy(void) { g_list = g_ble = g_wifi = g_bright_bar = g_bright_val = g_store = NULL; }

static void settings_update(const ui_snap_t *s)
{
    static const char *const BLE[] = {"idle", "searching", "connecting", "discovering", "starting ELM327", "logging", "locked (security on)"};
    const app_status_t *st = s->st;
    if (g_ble) {
        const char *state = st->ble <= BLE_ST_LOCKED ? BLE[st->ble] : "?";
        if (st->adapter_name[0]) ui_set_fmt(g_ble, "%.24s: %s%s", st->adapter_name, state, st->adapter_saved ? ", paired" : "");
        else ui_set_fmt(g_ble, "%s%s", state, st->adapter_saved ? ", paired" : ", none paired");
    }
    if (g_store) {
        if (s->fat.ok) ui_set_fmt(g_store, "%lu of %lu KB free", (unsigned long)s->fat.free_kb, (unsigned long)s->fat.total_kb);
        else ui_set_text(g_store, "measuring");
    }
    if (g_wifi) {
        static const char *const W[] = {"off", "scanning", "connecting", "connected", "idle between clock syncs"};
        ui_set_fmt(g_wifi, "%s%s", st->wifi <= WIFI_ST_IDLE ? W[st->wifi] : "?", st->time_synced ? ", clock set" : ", no clock");
    }
}

/* ======================= registry ======================= */
static const ocm_app_t APPS[] = {          /* launcher order and tile colours as on the E_Apps artboard; Settings last */
    { "drive",    "Drive",    LV_SYMBOL_GPS,       drive_create,    drive_update,    drive_destroy,    500, SEC_SKY,    0x8CC8FF, 0x1F6FEB, 0, &ocm_icon_drive_52, NULL, NULL },
    { "ai",       "AI",       LV_SYMBOL_EYE_OPEN,  ai_create,       ai_update,       NULL,             500, SEC_ORANGE, 0xFE860F, 0xE8590C, 0, &ocm_icon_ai_52, NULL, NULL },
    { "learn",    "Learning", LV_SYMBOL_REFRESH,   learn_create,    learn_update,    NULL,             1000, SEC_GREEN, 0x56F487, 0x2B8A3E, 0, &ocm_icon_learn_52, NULL, NULL },
    { "rev",      "R.E.V",    LV_SYMBOL_SHUFFLE,   rev_create,      rev_update,      NULL,             1000, SEC_GREEN, 0x56F487, 0x2B8A3E, 0, &ocm_icon_ai_52, NULL, &ocm_mark_rev_tile },
    { "summary",  "Summary",  LV_SYMBOL_IMAGE,     summary_create,  summary_update,  summary_destroy,  1000, SEC_SKY,   0xFF7AB6, 0xD6336C, 0, &ocm_icon_summary_52, NULL, NULL },
    { "live",     "Live",     LV_SYMBOL_PLAY,      live_create,     live_update,     NULL,             250, SEC_SKY,    0x72ECE4, 0x0C8599, 0, &ocm_icon_live_52, NULL, NULL },
    { "fuel",     "Fuel",     LV_SYMBOL_TINT,      fuel_create,     fuel_update,     NULL,             500, SEC_AMBER,  0xF2A13A, 0xD9480F, 0, &ocm_icon_fuel_52, NULL, NULL },
    { "brakes",   "Brakes",   LV_SYMBOL_STOP,      brakes_create,   brakes_update,   brakes_destroy,   1000, SEC_RED,   0xFD5A4A, 0xE03131, 0, &ocm_icon_brakes_52, NULL, NULL },
    { "trip",     "Trip",     LV_SYMBOL_LOOP,      trip_create,     trip_update,     NULL,             1000, SEC_CYAN,  0x38B8F6, 0x1971C2, 0, &ocm_icon_trip_52, NULL, NULL },
    { "engine",   "Engine",   LV_SYMBOL_WARNING,   engine_create,   engine_update,   NULL,             1000, SEC_AMBER, 0xFCCA21, 0xB8590A, 0, &ocm_icon_engine_52, NULL, NULL },
    { "faults",   "Faults",   LV_SYMBOL_WARNING,   faults_create,   faults_update,   faults_destroy,   1000, SEC_RED,   0xFD5A4A, 0xC92A2A, 0, &ocm_icon_faults_52, NULL, NULL },
    { "board",    "Board",    LV_SYMBOL_SD_CARD,   board_create,    board_update,    NULL,             1000, SEC_AQUA,  0x72ECE4, 0x087F5B, 0, &ocm_icon_board_52, NULL, NULL },
    { "files",    "Files",    LV_SYMBOL_DIRECTORY, files_create,    files_update,    files_destroy,    500, SEC_AQUA,   0xFFB38A, 0x3B5BDB, 0, &ocm_icon_files_52, NULL, NULL },
    { "settings", "Settings", LV_SYMBOL_SETTINGS,  settings_create, settings_update, settings_destroy, 1000, SEC_VIOLET, 0xB8B8B8, 0x5F6368, 0, &ocm_icon_settings_52, NULL, NULL },   /* keep last */
};

int ui_app_count(void) { return (int)(sizeof APPS / sizeof APPS[0]); }
const ocm_app_t *ui_app(int i) { return &APPS[i]; }
