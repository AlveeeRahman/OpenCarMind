/* Contrast check of the ESP-Mosaico palettes. Reads THEME_DARK and THEME_LIGHT straight from main/mosaico_display.c (the
   tables the firmware compiles, so this cannot drift from them) and checks every text/ground pair the apps draw for the
   WCAG 2.x AA ratio 4.5:1, and the graphic pairs (bars, class colours, page dots) for 3:1 (WCAG 1.4.11).
   Ratio = (L1 + 0.05) / (L2 + 0.05), L = 0.2126 R + 0.7152 G + 0.0722 B of the linearised sRGB channels
   (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055)^2.4). The light launcher's own literals (the HT/HM/HC_* macros) are
   checked on its glass (white at 184/255 over the #F3F1EC ground). usage: theme_tests [path to mosaico_display.c] [-v] */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, runs, verbose;
#define MAXF 160
typedef struct { char name[32]; unsigned v; } field_t;
typedef struct { const char *id; field_t f[MAXF]; int n; } table_t;

static double lin(unsigned c) { double s = c / 255.0; return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4); }
static double lum(unsigned h) { return 0.2126 * lin(h >> 16 & 255) + 0.7152 * lin(h >> 8 & 255) + 0.0722 * lin(h & 255); }
static double ratio(unsigned a, unsigned b) { double x = lum(a), y = lum(b); return x > y ? (x + 0.05) / (y + 0.05) : (y + 0.05) / (x + 0.05); }
static unsigned over(unsigned fg, int opa, unsigned bg)       /* fg at opa/255 over bg */
{
    unsigned o = 0;
    for (int s = 0; s <= 16; s += 8) o |= (unsigned)((((fg >> s) & 255) * opa + ((bg >> s) & 255) * (255 - opa) + 127) / 255) << s;
    return o;
}

/* ".name = 0xHEX" and ".name = { [SEC_X] = 0x.., .. }" / ".name = { 0x.., .. }" inside "static const ui_theme_t <id> = {" */
static void parse(const char *src, table_t *t)
{
    char head[64];
    snprintf(head, sizeof head, "static const ui_theme_t %s = {", t->id);
    const char *p = strstr(src, head), *end = p ? strstr(p, "\n};") : NULL;
    if (!p || !end) { printf("FAIL table %s not found\n", t->id); fails++; return; }
    for (p += strlen(head); p < end; p++) {
        if (*p != '.' || !isalpha((unsigned char)p[1])) continue;
        char nm[24]; int k = 0;
        for (p++; (isalnum((unsigned char)*p) || *p == '_') && k < 23; p++) nm[k++] = *p;
        nm[k] = 0;
        while (*p == ' ') p++;
        if (*p != '=') continue;
        for (p++; *p == ' '; p++) {}
        if (!strncmp(p, "0x", 2)) {
            if (t->n < MAXF) { snprintf(t->f[t->n].name, 32, "%s", nm); t->f[t->n++].v = (unsigned)strtoul(p, NULL, 16); }
        } else if (*p == '{') {
            const char *q = strchr(p, '}');
            int idx = 0;
            for (p++; p < q; p++) {
                char sub[16] = "";
                if (*p == '[') { int j = 0; for (p++; *p != ']' && j < 15; p++) sub[j++] = *p; sub[j] = 0; while (*p != '0' && p < q) p++; }
                if (!strncmp(p, "0x", 2) && t->n < MAXF) {
                    if (!sub[0]) snprintf(sub, sizeof sub, "%d", idx);
                    snprintf(t->f[t->n].name, 32, "%s.%s", nm, !strncmp(sub, "SEC_", 4) ? sub + 4 : sub);
                    t->f[t->n++].v = (unsigned)strtoul(p, (char **)&p, 16);
                    idx++;
                }
            }
        }
    }
}

static int get(const table_t *t, const char *name, unsigned *v)
{
    for (int i = 0; i < t->n; i++) if (!strcmp(t->f[i].name, name)) { *v = t->f[i].v; return 1; }
    printf("FAIL %s: no field %s\n", t->id, name);
    fails++;
    return 0;
}

static double worst_text = 99, worst_graphic = 99;
static void pair(const table_t *t, const char *fg, const char *bg, double min, const char *what)
{
    unsigned a, b;
    if (!get(t, fg, &a) || !get(t, bg, &b)) return;
    double r = ratio(a, b);
    runs++;
    if (min >= 4.5 && r < worst_text) worst_text = r;
    if (min < 4.5 && r < worst_graphic) worst_graphic = r;
    if (r + 1e-9 < min) { fails++; printf("FAIL %-5s %-14s #%06X on %-10s #%06X  %5.2f < %.1f  (%s)\n", t->id + 6, fg, a, bg, b, r, min, what); }
    else if (verbose) printf("ok   %-5s %-14s #%06X on %-10s #%06X  %5.2f  (%s)\n", t->id + 6, fg, a, bg, b, r, what);
}

static const char *SEC[] = {"section.SKY", "section.ORANGE", "section.GREEN", "section.PINK", "section.AMBER", "section.RED",
                            "section.AQUA", "section.VIOLET", "section.CYAN"};
#define N(a) (int)(sizeof a / sizeof a[0])

static void check(const table_t *t)
{
    static const char *TXT[] = {"text", "muted", "good", "warn", "bad", "estimate", "ai"};
    static const char *GND[] = {"bg", "card", "track", "banner", "sheet", "panel"};
    for (int g = 0; g < N(GND); g++) {
        for (int i = 0; i < N(TXT); i++) pair(t, TXT[i], GND[g], 4.5, "text and tones (events, notes, values)");
        for (int i = 0; i < N(SEC); i++) pair(t, SEC[i], GND[g], 4.5, "accent labels, captions, button labels");
    }
    for (int i = 0; i < N(SEC); i++) {
        pair(t, SEC[i], "chip", 4.5, "back chevron on its chip");
        pair(t, "bg", SEC[i], 4.5, "page colour on an accent fill: selected segment, roller row, Save");
        pair(t, SEC[i], "track", 3.0, "bar indicator on its track");
    }
    pair(t, "bg", "bad", 4.5, "confirm sheet: the red button's label");
    pair(t, "text", "track", 4.5, "Cancel, segment and dropdown labels");
    pair(t, "muted", "track", 4.5, "roller rows, dropdown");
    for (int k = 0; k < 4; k++) {
        char n[16];
        snprintf(n, sizeof n, "dcls.%d", k); pair(t, n, "panel", 4.5, "Drive: the class name");
        snprintf(n, sizeof n, "cls.%d", k);  pair(t, n, "card", 3.0, "class bars, timeline, legend squares");
    }
    pair(t, "label2", "panel", 4.5, "Drive labels and units");
    pair(t, "text", "panel", 4.5, "Drive values");
    pair(t, "muted", "panel", 4.5, "Drive: the class certainty");
    static const char *SB[] = {"sb_cpu", "sb_ai", "sb_mips", "sb_pwr"};
    for (int i = 0; i < N(SB); i++) { pair(t, SB[i], "bg", 4.5, "status row on the app page"); pair(t, SB[i], "card", 4.5, "status pill"); }
    pair(t, "good", "track", 3.0, "bars, tank, dots");
    pair(t, "text", "dim", 3.0, "page dots: current vs the others");
}

/* the light launcher's own colours (#define HT (s_light ? 0x1C1C1E : ..)) on its glass and on its ground */
static void home_literals(const char *src)
{
    unsigned glass = over(0xFFFFFF, 184, 0xF3F1EC);
    static const char *TXT[] = {"HT", "HM", "HC_CPU", "HC_C0", "HC_C1", "HC_PW", "HC_SKY", "HC_RED", "HC_WARN", "HC_GREEN"};
    for (int i = 0; i < N(TXT); i++) {
        char key[48];
        snprintf(key, sizeof key, "#define %s ", TXT[i]);
        const char *p = strstr(src, key);
        if (p) p = strstr(p, "s_light ? 0x");
        if (!p) { printf("FAIL home literal %s not found\n", TXT[i]); fails++; continue; }
        unsigned v = (unsigned)strtoul(p + 10, NULL, 16);
        double r = ratio(v, glass);
        runs++;
        if (r < worst_text) worst_text = r;
        if (r < 4.5) { fails++; printf("FAIL light home %-8s #%06X on glass #%06X  %5.2f < 4.5\n", TXT[i], v, glass, r); }
        else if (verbose) printf("ok   light home %-8s #%06X on glass #%06X  %5.2f\n", TXT[i], v, glass, r);
    }
}

int main(int argc, char **argv)
{
    const char *path = "../../main/mosaico_display.c";
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-v")) verbose = 1; else path = argv[i]; }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); return 1; }
    static char src[1 << 20];
    size_t n = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[n] = 0;

    /* the formula against known values: white on black 21, #767676 on white 4.54 (the classic AA grey) */
    runs++; if (fabs(ratio(0xFFFFFF, 0x000000) - 21.0) > 1e-9) { fails++; printf("FAIL ratio(white, black)\n"); }
    runs++; if (fabs(ratio(0x767676, 0xFFFFFF) - 4.54) > 0.01) { fails++; printf("FAIL ratio(#767676, white)\n"); }

    static table_t dark = { .id = "THEME_DARK" }, light = { .id = "THEME_LIGHT" };
    parse(src, &dark);
    parse(src, &light);
    unsigned flag;
    runs++; if (dark.n < 70 || light.n < 70) { fails++; printf("FAIL parsed %d / %d fields\n", dark.n, light.n); }
    runs++; if (!strstr(src, ".light = true") || (get(&light, "bg", &flag) && lum(flag) < 0.5)) { fails++; printf("FAIL light table is not light\n"); }
    check(&dark);
    check(&light);
    home_literals(src);
    printf("ui_theme: %d checks, %d failures (worst text pair %.2f:1, worst graphic pair %.2f:1)\n", runs, fails, worst_text, worst_graphic);
    return fails ? 1 : 0;
}
