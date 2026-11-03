#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "custom_pids.h"
#include "events.h"
#include <stdbool.h>

static const char *TAG = "custom_pid";

/* ------------------------------------------------------------ formula evaluator */
#define MAX_DEPTH 16   /* nesting of "(" and unary "-": bounds recursion on the OBD task stack */
typedef struct { const char *p; const uint8_t *b; int n; bool err; int depth; } ev_t;

static void skip_ws(ev_t *e) { while (*e->p == ' ' || *e->p == '\t') e->p++; }

static int var_index(ev_t *e)
{
    /* A..Z = 0..25, AA..AZ = 26..51, BA.. = 52.. */
    int idx = -1;
    while (isalpha((unsigned char)*e->p)) {
        int c = toupper((unsigned char)*e->p) - 'A';
        idx = (idx < 0) ? c : (idx + 1) * 26 + c;
        e->p++;
    }
    return idx;
}

static float byte_val(ev_t *e, int idx)
{
    if (idx < 0 || idx >= e->n) { e->err = true; return 0; }
    return e->b[idx];
}

static float parse_expr(ev_t *e);

static float parse_factor(ev_t *e)
{
    skip_ws(e);
    if ((*e->p == '-' || *e->p == '(') && ++e->depth > MAX_DEPTH) { e->err = true; return 0; }
    if (*e->p == '-') { e->p++; float v = -parse_factor(e); e->depth--; return v; }
    if (*e->p == '(') {
        e->p++;
        float v = parse_expr(e);
        e->depth--;
        skip_ws(e);
        if (*e->p == ')') e->p++; else e->err = true;
        return v;
    }
    if (*e->p == '{') {               /* {A:3} -> bit 3 of byte A */
        e->p++;
        int idx = var_index(e);
        float v = byte_val(e, idx);
        skip_ws(e);
        if (*e->p == ':') {
            e->p++;
            int bit = (int)strtol(e->p, (char **)&e->p, 10);
            v = ((int)v >> bit) & 1;
        }
        skip_ws(e);
        if (*e->p == '}') e->p++; else e->err = true;
        return v;
    }
    if (isalpha((unsigned char)*e->p)) return byte_val(e, var_index(e));
    if (isdigit((unsigned char)*e->p) || *e->p == '.') return strtof(e->p, (char **)&e->p);
    e->err = true;
    return 0;
}

static float parse_term(ev_t *e)
{
    float v = parse_factor(e);
    for (;;) {
        skip_ws(e);
        if (*e->p == '*') { e->p++; v *= parse_factor(e); }
        else if (*e->p == '/') { e->p++; float d = parse_factor(e); v = d != 0 ? v / d : 0; }
        else return v;
    }
}

static float parse_expr(ev_t *e)
{
    float v = parse_term(e);
    for (;;) {
        skip_ws(e);
        if (*e->p == '+') { e->p++; v += parse_term(e); }
        else if (*e->p == '-') { e->p++; v -= parse_term(e); }
        else return v;
    }
}

bool custom_pid_eval(const char *formula, const uint8_t *bytes, int n, float *result)
{
    ev_t e = { .p = formula, .b = bytes, .n = n, .err = false };
    float v = parse_expr(&e);
    skip_ws(&e);
    if (*e.p != 0) e.err = true;
    if (!e.err) *result = v;
    return !e.err;
}

/* ------------------------------------------------------------ CSV loader */
static void write_example(const char *path)
{
    char ex[160];
    snprintf(ex, sizeof ex, "%s", path);
    char *dot = strrchr(ex, '.');
    if (dot) strcpy(dot, ".example.csv");
    FILE *f = fopen(ex, "w");
    if (!f) return;
    fputs("# OpenCarMind custom PIDs. Copy this file to custom_pids.csv and edit. Max 8 rows.\n"
          "# Format (EngineLink style): ModePID,Description,Min,Max,Units,Formula,Header\n"
          "#  ModePID  = mode + PID/DID in hex, e.g. 0133 (mode 01 PID 33) or 22F40C (mode 22 DID F40C)\n"
          "#  Formula  = A,B,C.. are reply data bytes; + - * / ( ) and {A:3} for bit 3 of A\n"
          "#  Header   = 0 for functional (broadcast) addressing, or an 11-bit CAN ECU id such as 7E0\n"
          "# Tire pressure (TPMS) is manufacturer specific: look up your make's mode-22 DIDs and add them here.\n"
          "ModePID,Description,Min,Max,Units,Formula,Header\n"
          "0133,Baro,0,255,kPa,A,0\n"
          "010E,Timing adv,-64,63.5,deg,A/2-64,0\n", f);
    fclose(f);
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n' || e[-1] == '\t')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static bool all_hex(const char *s)
{
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return false;
    return true;
}

static bool read_only_service(const char *cmd)
{
    return strncmp(cmd, "01", 2) == 0 || strncmp(cmd, "09", 2) == 0 || strncmp(cmd, "21", 2) == 0 || strncmp(cmd, "22", 2) == 0;
}

size_t custom_pids_load(const char *path, custom_pid_t *out, size_t max)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        write_example(path);
        return 0;
    }
    char line[200];
    size_t n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (strncasecmp(line, "ModePID", 7) == 0) continue;
        char *f7[7] = {0};
        int k = 0;
        char *p = line;
        f7[k++] = p;
        while (k < 7 && (p = strchr(p, ',')) != NULL) { *p++ = 0; f7[k++] = p; }
        if (k < 6) { ESP_LOGW(TAG, "skip malformed line: %s", line); continue; }
        for (int i = 0; i < k; i++) trim(f7[i]);
        custom_pid_t *c = &out[n];
        memset(c, 0, sizeof *c);
        /* reject, never truncate: a cut command is a different request, a cut header a different ECU */
        if (strlen(f7[0]) >= sizeof c->cmd || strlen(f7[5]) >= sizeof c->formula ||
            (k == 7 && strlen(f7[6]) >= sizeof c->header)) {
            sys_event(EV_WARN, "Custom PID %.12s refused: a field is too long", f7[0]);
            continue;
        }
        snprintf(c->cmd, sizeof c->cmd, "%s", f7[0]);
        snprintf(c->name, sizeof c->name, "%s", f7[1]);
        snprintf(c->unit, sizeof c->unit, "%s", f7[4]);
        snprintf(c->formula, sizeof c->formula, "%s", f7[5]);
        if (k == 7 && f7[6][0] && strcmp(f7[6], "0") != 0) snprintf(c->header, sizeof c->header, "%s", f7[6]);
        /* reply prefix: (mode + 0x40) followed by the PID digits */
        size_t L = strlen(c->cmd);
        if (L < 4 || L % 2 || !all_hex(c->cmd)) { ESP_LOGW(TAG, "bad ModePID %s", c->cmd); continue; }
        /* read-only services only: 01 current data, 09 vehicle info, 21/22 read data by local id / identifier. This file
           and the remote config can otherwise send any request to any ECU, ECU resets and writes included. */
        if (!read_only_service(c->cmd)) { sys_event(EV_WARN, "Custom PID %s refused: not a read-only service", c->cmd); continue; }
        size_t H = strlen(c->header);
        if (H && !((H == 3 || H == 6) && all_hex(c->header))) { sys_event(EV_WARN, "Custom PID header %s refused", c->header); continue; }
        char m[3] = {c->cmd[0], c->cmd[1], 0};
        int mode = (int)strtol(m, NULL, 16) + 0x40;
        snprintf(c->prefix, sizeof c->prefix, "%02X%s", mode, c->cmd + 2);
        for (char *q = c->prefix; *q; q++) *q = toupper((unsigned char)*q);
        ESP_LOGI(TAG, "custom %u: %s '%s' [%s] = %s hdr=%s", (unsigned)n, c->cmd, c->name, c->unit, c->formula, c->header[0] ? c->header : "func");
        n++;
    }
    fclose(f);
    return n;
}
