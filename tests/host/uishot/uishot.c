/* ESP-Mosaico screens on the Mac: the launcher shell (mosaico_display.c, included here so its static builders can be
   called) and every app (mosaico_apps.c) built with LVGL 9.5 on a 480 x 480 RGB565 display, in the dark and the light
   palette, with the widest values the firmware can publish (fw_stubs.c). For every visible label it measures the text
   with LVGL's own text code (the font, letter and line spacing in force) and reports:
     CUT     a one-line label (clip mode) narrower than its text; DOTS (listed): a dots label cut with "..." by design
     WRAPS   a dots label whose height follows its content: LVGL wraps it instead of cutting it
     CLIP    text outside the area of an ancestor that clips it (sideways always; up/down unless it scrolls)
     PAD     text in its parent's padding (touches the card edge)
     OVER    two texts overlap, or a text runs over a small drawn shape beside it (a lock, bars, a dot)
     TIGHT   two texts on one line less than 4 px apart
     CORNER  text outside the rounded screen's safe area (UISHOT_CORNER_R, inset UISHOT_CORNER_M); SCORNR (listed, not a
             failure): a row of a scrolling list that crosses a corner at the scroll position shot
     SMALL   a font under 16 px
   It exits 1 on any of them. --png DIR also writes DIR/<screen>_<theme>.png (the glass outside the corner radius is
   darkened); --list prints every label with its font, text width, label width and margin (free px on its line to the
   nearest text or drawn edge); --measure <px> <letter space> "text"... prints text widths only.
   Limits: the firmware values are the stubs' hand-picked worst cases, not every reachable state; the corner radius is
   an estimate from a photo; overlaps with large shapes and anything about timing are out of scope; it is the Mac, not
   the board: the result says what LVGL lays out, the panel still has to be checked. */
#include "../../main/mosaico_display.c"
#include <zlib.h>
#include "src/misc/lv_area_private.h"
#include "src/display/lv_display_private.h"
#include "src/misc/lv_text_private.h"
#include "uishot.h"

#define UISHOT_CORNER_R 60   /* the panel's visible corner radius, px: estimate from the owner's photo (52-65 px) */
#define UISHOT_CORNER_M 4    /* text keeps this far inside the rounded edge */
#define FLOOR_PX 16

static uint16_t s_fb[UI_W * UI_H];
static int s_fails, s_warns;
static bool s_list;
static const char *s_png_dir;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *p = (const uint16_t *)px;
    int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            if (x >= 0 && x < UI_W && y >= 0 && y < UI_H) s_fb[y * UI_W + x] = p[(y - a->y1) * w + (x - a->x1)];
    lv_display_flush_ready(d);
}

/* ---- PNG (zlib, filter 0) with the corner mask drawn as a thin line ---- */
static void be32(uint8_t *b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    be32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)type, 4);
    c = crc32(c, d, n);
    be32(h, (uint32_t)c);
    fwrite(h, 1, 4, f);
}
static bool corner_out(int x, int y, int r, int m);
static void write_png(const char *path)
{
    size_t raw_n = (size_t)UI_H * (1 + UI_W * 3);
    uint8_t *raw = malloc(raw_n), *z = malloc(compressBound(raw_n));
    for (int y = 0; y < UI_H; y++) {
        uint8_t *r = raw + (size_t)y * (1 + UI_W * 3);
        *r++ = 0;
        for (int x = 0; x < UI_W; x++) {
            uint16_t c = s_fb[y * UI_W + x];
            uint8_t R = (uint8_t)(((c >> 11) & 31) * 255 / 31), G = (uint8_t)(((c >> 5) & 63) * 255 / 63), B = (uint8_t)((c & 31) * 255 / 31);
            if (corner_out(x, y, UISHOT_CORNER_R, 0)) { R /= 4; G /= 4; B /= 4; }   /* what the glass hides, darkened */
            *r++ = R; *r++ = G; *r++ = B;
        }
    }
    uLongf zn = compressBound(raw_n);
    compress2(z, &zn, raw, raw_n, 6);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13];
    be32(ih, UI_W); be32(ih + 4, UI_H); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
}

/* ---- the rounded screen: (x, y) outside the rounded rectangle inset by m (corner radius r - m) ---- */
static bool corner_out(int x, int y, int r, int m)
{
    if (x < m || y < m || x >= UI_W - m || y >= UI_H - m) return true;
    int cx = x < r ? r : x >= UI_W - r ? UI_W - 1 - r : x;
    int cy = y < r ? r : y >= UI_H - r ? UI_H - 1 - r : y;
    if (cx == x || cy == y) return false;
    double dx = x - cx, dy = y - cy, rr = r - m;
    return dx * dx + dy * dy > rr * rr;
}
/* how far a box reaches outside the safe area (0 = inside), checked on its outline */
static int corner_excess(const lv_area_t *b)
{
    int worst = 0;
    for (int x = b->x1; x <= b->x2; x++)
        for (int k = 0; k < 2; k++) {
            int y = k ? b->y2 : b->y1;
            for (int e = 0; e < 40 && corner_out(x, y, UISHOT_CORNER_R, UISHOT_CORNER_M - e); e++) if (e + 1 > worst) worst = e + 1;
        }
    for (int y = b->y1; y <= b->y2; y++)
        for (int k = 0; k < 2; k++) {
            int x = k ? b->x2 : b->x1;
            for (int e = 0; e < 40 && corner_out(x, y, UISHOT_CORNER_R, UISHOT_CORNER_M - e); e++) if (e + 1 > worst) worst = e + 1;
        }
    return worst;
}

static int font_px(const lv_font_t *f)
{
    static const struct { const lv_font_t *f; int px; } F[] = {
        {&lv_font_montserrat_8, 8}, {&lv_font_montserrat_10, 10}, {&lv_font_montserrat_12, 12}, {&lv_font_montserrat_14, 14},
        {&lv_font_montserrat_16, 16}, {&lv_font_montserrat_18, 18}, {&lv_font_montserrat_20, 20}, {&lv_font_montserrat_22, 22},
        {&lv_font_montserrat_24, 24}, {&lv_font_montserrat_26, 26}, {&lv_font_montserrat_28, 28}, {&lv_font_montserrat_30, 30},
        {&lv_font_montserrat_32, 32}, {&lv_font_montserrat_34, 34}, {&lv_font_montserrat_36, 36}, {&lv_font_montserrat_38, 38},
        {&lv_font_montserrat_40, 40}, {&lv_font_montserrat_42, 42}, {&lv_font_montserrat_44, 44}, {&lv_font_montserrat_46, 46},
        {&lv_font_montserrat_48, 48}};
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) if (F[i].f == f) return F[i].px;
    return f ? -(int)f->line_height : 0;
}

/* the glyphs' ink inside a line box: px from the line top to the highest ink and to the lowest ink */
static void ink_rows(const char *t, const lv_font_t *f, int *top, int *bot)
{
    int lh = lv_font_get_line_height(f), base = f->base_line, tp = lh, bt = 0;
    uint32_t i = 0;
    while (t[i]) {
        uint32_t ch = lv_text_encoded_next(t, &i), nx = 0;
        uint32_t j = i;
        if (t[j]) nx = lv_text_encoded_next(t, &j);
        lv_font_glyph_dsc_t g;
        if (ch == ' ' || ch == '\n' || !lv_font_get_glyph_dsc(f, &g, ch, nx) || !g.box_h) continue;
        int y1 = lh - base - g.box_h - g.ofs_y, y2 = lh - base - g.ofs_y;
        if (y1 < tp) tp = y1;
        if (y2 > bt) bt = y2;
    }
    if (tp > bt) { tp = 0; bt = lh; }
    *top = tp; *bot = bt;
}

static bool visible(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
        if (lv_obj_get_style_opa(o, 0) == LV_OPA_TRANSP) return false;
    }
    return true;
}

typedef struct { lv_obj_t *o; lv_area_t tb, clip; char txt[64]; int px, w, room; } shown_t;
static shown_t s_shown[600];
static int s_nshown;
static char s_screen[48];
static const char *s_theme;

static void report(const char *kind, bool fail, lv_obj_t *o, const char *txt, int px, const char *fmt, ...)
{
    (void)o;
    char m[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    printf("%-6s %s/%s  \"%.48s\" %dpx: %s\n", kind, s_screen, s_theme, txt, px, m);
    if (fail) s_fails++; else s_warns++;
}

static void check_label(lv_obj_t *o)
{
    const char *t = lv_label_get_text(o);
    if (!t || !t[0]) return;
    const lv_font_t *f = lv_obj_get_style_text_font(o, 0);
    int ls = lv_obj_get_style_text_letter_space(o, 0), lsp = lv_obj_get_style_text_line_space(o, 0);
    int px = font_px(f);
    char txt[64];
    snprintf(txt, sizeof txt, "%s", t);
    for (char *c = txt; *c; c++) if (*c == '\n') *c = '|';
    lv_area_t ca;
    lv_obj_get_content_coords(o, &ca);
    int cw = lv_area_get_width(&ca), ch = lv_area_get_height(&ca);
    lv_point_t nat, wr;
    lv_text_get_size(&nat, t, f, ls, lsp, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_label_long_mode_t lm = lv_label_get_long_mode(o);
    lv_text_get_size(&wr, t, f, ls, lsp, lm == LV_LABEL_LONG_WRAP ? cw : LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int tw = lm == LV_LABEL_LONG_WRAP ? wr.x : nat.x, th = lm == LV_LABEL_LONG_WRAP ? wr.y : nat.y;
    if (tw > cw) tw = cw;
    lv_text_align_t al = lv_obj_get_style_text_align(o, 0);
    lv_area_t tb = { ca.x1, ca.y1, ca.x1 + tw - 1, ca.y1 + (th < ch || lm == LV_LABEL_LONG_WRAP ? th : ch) - 1 };
    if (al == LV_TEXT_ALIGN_CENTER) { tb.x1 = ca.x1 + (cw - tw) / 2; tb.x2 = tb.x1 + tw - 1; }
    else if (al == LV_TEXT_ALIGN_RIGHT) { tb.x2 = ca.x2; tb.x1 = ca.x2 - tw + 1; }

    if (px > 0 && px < FLOOR_PX) report("SMALL", true, o, txt, px, "font under %d px", FLOOR_PX);
    if (px <= 0) report("SMALL", true, o, txt, px, "not a Montserrat size (line %d)", -px);
    int margin = 9999;
    if (lm != LV_LABEL_LONG_WRAP && nat.x > cw && lm != LV_LABEL_LONG_MODE_SCROLL && lm != LV_LABEL_LONG_MODE_SCROLL_CIRCULAR) {
        /* a label set to dots on purpose (names of any length) cuts by design: listed, not a failure. LVGL 9.5 wraps a
           dots label whose height follows its content instead of cutting it: that is a failure (lines run into what
           is below) */
        bool wraps = lm == LV_LABEL_LONG_DOT && lv_obj_get_style_height(o, 0) == LV_SIZE_CONTENT && !strchr(t, '\n');
        if (wraps) report("WRAPS", true, o, txt, px, "dots label of content height: %d px of text wraps in %d px", (int)nat.x, cw);
        else report(lm == LV_LABEL_LONG_DOT ? "DOTS" : "CUT", lm != LV_LABEL_LONG_DOT, o, txt, px, "text %d px in a %d px label", (int)nat.x, cw);
        margin = cw - nat.x;
    } else if (lm != LV_LABEL_LONG_WRAP && lv_obj_get_style_width(o, 0) != LV_SIZE_CONTENT) margin = cw - nat.x;
    if (lm == LV_LABEL_LONG_WRAP && lv_obj_get_style_height(o, 0) != LV_SIZE_CONTENT && wr.y > ch)
        report("CUT", true, o, txt, px, "wrapped text %d px high in a %d px label", (int)wr.y, ch);

    /* clipping by ancestors; a vertically scrolling ancestor shows the rest by scrolling. Text scrolled away or on
       another launcher tile is not on the screen: nothing to check */
    lv_area_t vis = tb;
    lv_obj_t *par = lv_obj_get_parent(o);
    for (lv_obj_t *a = par; a; a = lv_obj_get_parent(a)) {
        if (lv_obj_has_flag(a, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) continue;
        lv_area_t ac, big;
        lv_obj_get_coords(a, &ac);
        big = ac;
        if (lv_obj_has_flag(a, LV_OBJ_FLAG_SCROLLABLE)) { big.x1 -= 2000; big.x2 += 2000; }
        lv_area_t probe = tb;
        if (!lv_area_intersect(&probe, &probe, lv_obj_has_flag(a, LV_OBJ_FLAG_SCROLLABLE) ? &ac : &big)) return;
    }
    bool in_scroll = false;
    for (lv_obj_t *a = par; a; a = lv_obj_get_parent(a)) {
        if (lv_obj_has_flag(a, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) continue;
        lv_area_t ac;
        lv_obj_get_coords(a, &ac);
        bool vs = lv_obj_has_flag(a, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(a) & LV_DIR_VER) &&
                  (lv_obj_get_scroll_top(a) > 0 || lv_obj_get_scroll_bottom(a) > 0);
        bool hs = lv_obj_has_flag(a, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(a) & LV_DIR_HOR) &&
                  (lv_obj_get_scroll_left(a) > 0 || lv_obj_get_scroll_right(a) > 0);
        if (in_scroll) vs = true;                         /* above a scroller, up/down is the scroll position */
        if (vs || hs) in_scroll = true;
        int dl = ac.x1 - tb.x1, dr = tb.x2 - ac.x2, du = ac.y1 - tb.y1, dd = tb.y2 - ac.y2;
        int mh = -(dl > dr ? dl : dr);
        if (!hs && mh < margin) margin = mh;
        if (!hs && (dl > 0 || dr > 0) && tb.x2 >= ac.x1 && tb.x1 <= ac.x2)
            report("CLIP", true, o, txt, px, "%d px outside an ancestor sideways (ancestor x %d..%d)", dl > dr ? dl : dr, (int)ac.x1, (int)ac.x2);
        if (!vs && (du > 0 || dd > 0) && tb.y2 >= ac.y1 && tb.y1 <= ac.y2)
            report("CLIP", true, o, txt, px, "%d px outside an ancestor up/down (ancestor y %d..%d)", du > dd ? du : dd, (int)ac.y1, (int)ac.y2);
        if (!lv_area_intersect(&vis, &vis, &ac)) return;              /* scrolled away or on another tile */
    }
    if (par && !lv_obj_has_flag(par, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_area_t pc;
        lv_obj_get_content_coords(par, &pc);
        int pl = pc.x1 - tb.x1, pr = tb.x2 - pc.x2;
        if ((pl > 0 || pr > 0) && lv_obj_get_style_pad_left(par, 0) + lv_obj_get_style_pad_right(par, 0) > 0)
            report("PAD", true, o, txt, px, "%d px into its parent's padding", pl > pr ? pl : pr);
    }
    int it0, ib0;
    ink_rows(t, f, &it0, &ib0);
    lv_area_t vink = vis;
    if (tb.y1 + it0 > vink.y1) vink.y1 = tb.y1 + it0;
    if (tb.y2 - (lv_font_get_line_height(f) - ib0) < vink.y2) vink.y2 = tb.y2 - (lv_font_get_line_height(f) - ib0);
    if (vink.y2 < vink.y1) vink.y2 = vink.y1;
    int ce = corner_excess(&vink);
    /* a row of a scrolling list crosses the corners while it scrolls: listed; at rest (top or end) it must not */
    if (ce) report(in_scroll ? "SCORNR" : "CORNER", !in_scroll, o, txt, px, "%d px past the safe edge%s (box %d,%d..%d,%d)", ce,
                   in_scroll ? " (a scrolling list, this scroll position)" : "",
                   (int)vis.x1, (int)vis.y1, (int)vis.x2, (int)vis.y2);
    (void)margin;
    /* text over a small drawn shape beside it in the same parent (a lock, signal bars, a dot, a trash button) */
    if (par) {
        int it1, ib1;
        ink_rows(t, f, &it1, &ib1);
        lv_area_t ink = vis;
        if (tb.y1 + it1 > ink.y1) ink.y1 = tb.y1 + it1;
        if (tb.y2 - (lv_font_get_line_height(f) - ib1) < ink.y2) ink.y2 = tb.y2 - (lv_font_get_line_height(f) - ib1);
        for (uint32_t i = 0; i < lv_obj_get_child_count(par); i++) {
            lv_obj_t *sb = lv_obj_get_child(par, (int32_t)i);
            if (sb == o || lv_obj_check_type(sb, &lv_label_class) || !visible(sb)) continue;
            lv_area_t sa, x;
            lv_obj_get_coords(sb, &sa);
            if (lv_area_get_width(&sa) > 64 || lv_area_get_height(&sa) > 64) continue;
            bool drawn = lv_obj_check_type(sb, &lv_image_class) || lv_obj_get_style_bg_opa(sb, 0) > LV_OPA_TRANSP ||
                         lv_obj_get_style_border_width(sb, 0) > 0;
            if (drawn && lv_area_intersect(&x, &ink, &sa) && lv_area_get_width(&x) >= 2 && lv_area_get_height(&x) >= 2)
                report("OVER", true, o, txt, px, "text over a %dx%d shape beside it at %d,%d", (int)lv_area_get_width(&sa),
                       (int)lv_area_get_height(&sa), (int)sa.x1, (int)sa.y1);
        }
    }
    if (s_nshown < (int)(sizeof s_shown / sizeof s_shown[0])) {
        /* overlaps are judged on the ink: the line box of a big font has empty rows above its capitals */
        int it, ib;
        ink_rows(t, f, &it, &ib);
        lv_area_t ink = vis;
        if (tb.y1 + it > ink.y1) ink.y1 = tb.y1 + it;
        if (tb.y2 - (lv_font_get_line_height(f) - ib) < ink.y2) ink.y2 = tb.y2 - (lv_font_get_line_height(f) - ib);
        if (ink.y2 < ink.y1) ink.y2 = ink.y1;
        /* the room around the text: up to the nearest drawn edge (a card's border or fill, the screen) */
        lv_area_t clip = { 0, 0, UI_W - 1, UI_H - 1 };
        for (lv_obj_t *a = par; a; a = lv_obj_get_parent(a)) {
            if (!lv_obj_get_parent(a) || lv_obj_get_style_bg_opa(a, 0) > LV_OPA_TRANSP || lv_obj_get_style_border_width(a, 0) > 0) {
                lv_area_t ac;
                lv_obj_get_coords(a, &ac);
                if (ac.x1 > clip.x1) clip.x1 = ac.x1;
                if (ac.x2 < clip.x2) clip.x2 = ac.x2;
                break;
            }
        }
        s_shown[s_nshown].clip = clip;
        s_shown[s_nshown].px = px; s_shown[s_nshown].w = (int)nat.x; s_shown[s_nshown].room = cw;
        s_shown[s_nshown].o = o; s_shown[s_nshown].tb = ink;
        snprintf(s_shown[s_nshown].txt, sizeof s_shown[s_nshown].txt, "%s", txt);
        s_nshown++;
    }
}

static void check_font_obj(lv_obj_t *o, const char *what)
{
    int px = font_px(lv_obj_get_style_text_font(o, 0));
    if (px < FLOOR_PX) report("SMALL", true, o, what, px, "%s text under %d px", what, FLOOR_PX);
}

/* draw order: a full-screen opaque panel (the events list, a picker page) hides everything drawn before it */
static int s_dfs, s_cover;
static void find_cover(lv_obj_t *o)
{
    if (!visible(o)) return;
    int me = s_dfs++;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    if (lv_obj_get_parent(o) && a.x1 <= 0 && a.y1 <= 0 && a.x2 >= UI_W - 1 && a.y2 >= UI_H - 1 &&
        lv_obj_get_style_bg_opa(o, 0) == LV_OPA_COVER && lv_obj_get_style_opa(o, 0) == LV_OPA_COVER) s_cover = me;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) find_cover(lv_obj_get_child(o, (int32_t)i));
}

static void walk(lv_obj_t *o)
{
    if (!visible(o)) return;
    int me = s_dfs++;
    if (me < s_cover) {                                    /* under the cover: only its later subtrees can show */
        for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) walk(lv_obj_get_child(o, (int32_t)i));
        return;
    }
    if (lv_obj_check_type(o, &lv_label_class)) check_label(o);
    else if (lv_obj_check_type(o, &lv_buttonmatrix_class)) check_font_obj(o, "button matrix");
    else if (lv_obj_check_type(o, &lv_roller_class)) check_font_obj(o, "roller");
    else if (lv_obj_check_type(o, &lv_dropdown_class)) check_font_obj(o, "dropdown");
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) walk(lv_obj_get_child(o, (int32_t)i));
}

static void overlaps(int from, int to)
{
    for (int i = from; i < to; i++)
        for (int j = i + 1; j < to; j++) {
            lv_area_t x;
            const lv_area_t *A = &s_shown[i].tb, *B = &s_shown[j].tb;
            int vo = (A->y2 < B->y2 ? A->y2 : B->y2) - (A->y1 > B->y1 ? A->y1 : B->y1);
            int gap = A->x2 < B->x1 ? B->x1 - A->x2 - 1 : B->x2 < A->x1 ? A->x1 - B->x2 - 1 : -1;
            if (vo >= 4 && gap >= 0 && gap < 4) {             /* on one line with no space between: reads as one word */
                printf("TIGHT  %s/%s  \"%.40s\" and \"%.40s\": %d px apart\n", s_screen, s_theme, s_shown[i].txt, s_shown[j].txt, gap);
                s_fails++;
                continue;
            }
            if (!lv_area_intersect(&x, &s_shown[i].tb, &s_shown[j].tb)) continue;
            if (lv_area_get_width(&x) < 2 || lv_area_get_height(&x) < 2) continue;
            printf("OVER   %s/%s  \"%.40s\" and \"%.40s\": %dx%d px\n", s_screen, s_theme, s_shown[i].txt, s_shown[j].txt,
                   (int)lv_area_get_width(&x), (int)lv_area_get_height(&x));
            s_fails++;
        }
}

/* --list: each text, its width and its free space on its line: to the nearest text beside it or the edge of what it
   is drawn on (a card, the screen) */
static void list_margins(int from, int to)
{
    for (int i = from; i < to; i++) {
        const shown_t *e = &s_shown[i];
        int l = e->tb.x1 - e->clip.x1, r = e->clip.x2 - e->tb.x2;
        for (int j = from; j < to; j++) {
            if (j == i) continue;
            const lv_area_t *B = &s_shown[j].tb;
            int vo = (e->tb.y2 < B->y2 ? e->tb.y2 : B->y2) - (e->tb.y1 > B->y1 ? e->tb.y1 : B->y1);
            if (vo < 4) continue;
            if (B->x1 > e->tb.x2 && B->x1 - e->tb.x2 - 1 < r) r = B->x1 - e->tb.x2 - 1;
            if (B->x2 < e->tb.x1 && e->tb.x1 - B->x2 - 1 < l) l = e->tb.x1 - B->x2 - 1;
        }
        printf("LABEL  %s/%s  \"%.48s\" %dpx w %d room %d margin %d (left %d, right %d)\n", s_screen, s_theme, e->txt, e->px, e->w,
               e->room, l < r ? l : r, l, r);
    }
}

static void shoot(const char *name)
{
    snprintf(s_screen, sizeof s_screen, "%s", name);
    for (int k = 0; k < 4; k++) { lv_tick_inc(20); lv_timer_handler(); slide_tick(); }
#if UI_BLIT_SLIDE
    s_blit_on = false;
#endif
    if (s_sliding) slide_set(false);
    lv_display_enable_invalidation(s_disp, true);
    lv_obj_invalidate(lv_screen_active());
    lv_obj_update_layout(lv_screen_active());
    lv_obj_update_layout(lv_layer_top());
    lv_refr_now(NULL);
    s_nshown = 0;
    s_dfs = 0; s_cover = 0;
    find_cover(lv_screen_active());
    s_dfs = 0;
    walk(lv_screen_active());
    s_dfs = 0; s_cover = 0;
    int n_scr = s_nshown;
    overlaps(0, n_scr);
    s_dfs = 0; s_cover = 0;
    find_cover(lv_layer_top());
    s_dfs = 0;
    walk(lv_layer_top());
    s_dfs = 0; s_cover = 0;
    int n_top = s_nshown;
    /* the banner sits over the page on purpose: overlaps inside the status bar and the banner only */
    overlaps(n_scr, n_top);
    if (s_list) { list_margins(0, n_scr); list_margins(n_scr, n_top); }
    if (s_png_dir) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s_%s.png", s_png_dir, name, s_theme);
        write_png(p);
    }
}

/* a page that scrolls: also its end, the first vertical scroller with content below */
static lv_obj_t *find_scroller(lv_obj_t *o)
{
    if (!visible(o)) return NULL;
    if (!lv_obj_check_type(o, &lv_label_class) && lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(o) & LV_DIR_VER) &&
        lv_obj_get_scroll_bottom(o) > 0) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *f = find_scroller(lv_obj_get_child(o, (int32_t)i));
        if (f) return f;
    }
    return NULL;
}
static void shoot_scrolled(const char *name)
{
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t *sc = find_scroller(lv_screen_active());
    if (!sc) return;
    lv_area_t a;
    lv_obj_get_coords(sc, &a);
    printf("SCROLL %s/%s  box %d,%d %dx%d, %d px more below\n", name, s_theme, (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a),
           (int)lv_area_get_height(&a), (int)lv_obj_get_scroll_bottom(sc));
    char n[48];
    for (int step = 1; step <= 6 && lv_obj_get_scroll_bottom(sc) > 0; step++) {
        lv_obj_scroll_by_bounded(sc, 0, -(lv_obj_get_height(sc) - 40), LV_ANIM_OFF);   /* stops at the end, as a finger does */
        lv_obj_update_layout(sc);
        snprintf(n, sizeof n, "%s_s%d", name, step);
        shoot(n);
    }
    lv_obj_scroll_to_y(sc, 0, LV_ANIM_OFF);
}

void uishot_events_list(void);
void uishot_event_detail(void);
void uishot_file_detail(void);
void uishot_clock_picker(void);
void uishot_wifi_password(void);
void uishot_model_refused(void);
void uishot_close_overlays(void);

static int app_index(const char *id)
{
    for (int i = 0; i < ui_app_count(); i++) if (!strcmp(ui_app(i)->id, id)) return i;
    fprintf(stderr, "no app %s\n", id);
    exit(2);
}

/* the pages and sheets a tap opens */
static void overlays(void)
{
    open_app(app_index("summary"));
    for (int k = 0; k < 4; k++) { lv_tick_inc(20); lv_timer_handler(); }   /* the summary pages build in an lv_async call */
    uishot_events_list();
    shoot("events_list");
    shoot_scrolled("events_list");
    uishot_event_detail();
    shoot("event_detail");
    open_app(app_index("files"));
    uishot_file_detail();
    take_snapshot();
    ui_app(app_index("files"))->update(&s_snap);
    shoot("file_detail");
    shoot_scrolled("file_detail");
    open_app(app_index("settings"));
    uishot_clock_picker();
    shoot("clock_picker");
    uishot_close_overlays();
    open_app(app_index("wifi"));
    uishot_wifi_password();
    shoot("wifi_password");
    uishot_close_overlays();
    uishot_model_refused();
    shoot("confirm_sheet");
    modal_close(NULL);
    ui_go_home();
    power_sheet_cb(NULL);
    shoot("power_sheet");
    if (s_psheet) { lv_obj_delete(s_psheet); s_psheet = NULL; }
    /* the OBD adapter pairing sheet: a usual name, and the longest printable name (23 characters, no spaces) */
    static const char *const PAIR_NAMES[] = {"OBDLink CX", "WWWWWWWWWWWWWWWWWWWWWWW"};
    for (int k = 0; k < 2; k++) {
        uishot_case.obd_pair = PAIR_NAMES[k];
        s_pair_done = 0;
        pair_sheet_refresh();
        shoot(k ? "pair_sheet_long" : "pair_sheet");
        if (s_pair_sheet) { lv_obj_delete(s_pair_sheet); s_pair_sheet = NULL; }
    }
    uishot_case.obd_pair = NULL;
}

static void set_theme(bool light)
{
    s_light = light;
    ui_th = light ? &THEME_LIGHT : &THEME_DARK;
    s_theme = light ? "light" : "dark";
    rebuild_all();
    if (s_wall_ready && s_wall_img) lv_image_set_src(s_wall_img, &s_wall_dsc2[s_light]);
}

static void all_screens(const char *suf)
{
    take_snapshot();
    refresh_status_bar(&s_snap);
    if (s_cur < 0) refresh_home(&s_snap);
    for (int t = 0; t < s_ntiles; t++) {
        ui_go_home();
        lv_tileview_set_tile_by_index(s_tv, (uint32_t)t, 0, LV_ANIM_OFF);
        char n[24];
        snprintf(n, sizeof n, t ? "apps%d%s" : "home%.0d%s", t, suf);
        shoot(n);
    }
    for (int i = 0; i < ui_app_count(); i++) {
        open_app(i);
        char n[40];
        snprintf(n, sizeof n, "%s%s", ui_app(i)->id, suf);
        shoot(n);
        shoot_scrolled(n);
    }
    ui_go_home();
}

int main(int argc, char **argv)
{
    bool check = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check = true;
        else if (!strcmp(argv[i], "--list")) s_list = true;
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) s_png_dir = argv[++i];
    }
    (void)check;
    lv_init();
    if (argc >= 5 && !strcmp(argv[1], "--measure")) {          /* --measure <px> <letter space> "text" ...: widths only */
        static const lv_font_t *const MF[] = {&lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
            &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
            &lv_font_montserrat_28, &lv_font_montserrat_32, &lv_font_montserrat_40, &lv_font_montserrat_48};
        int px = atoi(argv[2]), ls = atoi(argv[3]);
        const lv_font_t *f = NULL;
        for (size_t k = 0; k < sizeof MF / sizeof MF[0]; k++) if (font_px(MF[k]) == px) f = MF[k];
        if (!f) { fprintf(stderr, "no %d px font\n", px); return 2; }
        for (int i = 4; i < argc; i++) {
            lv_point_t sz;
            lv_text_get_size(&sz, argv[i], f, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            printf("%4d x %2d  \"%s\"\n", (int)sz.x, (int)sz.y, argv[i]);
        }
        return 0;
    }
    s_disp = lv_display_create(UI_W, UI_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    static uint8_t buf[UI_W * UI_H * 2];
    lv_display_set_buffers(s_disp, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    uishot_case.model = ML_MODEL_REV;
    uishot_case.health = OS_H_DEGRADED;
    uishot_case.runlevel = OS_RL_UPDATE;
    uishot_fill();
    settings_load();
    s_producers_ready = true;
    s_home = make_screen();
    s_appscr = make_screen();
    s_body = lv_obj_create(s_appscr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, UI_W, UI_H);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    wall_task(NULL);
    for (int th = 0; th < 2; th++) {
        set_theme(th == 1);
        lv_screen_load(s_home);
        all_screens("");
        /* the critical-alarm and pairing banners over an app page */
        uishot_case.alarm = true;
        uishot_fill();
        open_app(0);
        shoot("banner_alarm");
        uishot_case.alarm_desc = "Crank and cam circuits both faulted, see Faults";
        uishot_fill();
        open_app(0);
        shoot("banner_alarm_long");
        uishot_case.alarm = false; uishot_case.alarm_desc = NULL; uishot_case.pairing = true;
        uishot_fill();
        open_app(0);
        shoot("banner_pairing");
        uishot_case.pairing = false;
        uishot_fill();
        overlays();
        uishot_case.pairing = true;
        uishot_fill();
        open_app(0);
        shoot("banner_pairing");
        uishot_case.pairing = false;
        uishot_case.idle = true; uishot_case.no_clock = true;
        uishot_fill();
        all_screens("_idle");
        uishot_case.idle = false; uishot_case.no_clock = false;
        uishot_fill();
    }
    printf("textfit: %d problem(s); listed only: %d name(s) cut with dots by design or rows crossing a corner mid-scroll%s\n", s_fails, s_warns, s_fails ? "" : "; every other text fits");
    return s_fails ? 1 : 0;
}

