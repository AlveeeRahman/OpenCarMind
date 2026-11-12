#include "ui_events_logic.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char SAVE_TEXT[] = "Safe to turn off";     /* sdlog.c: written at every save that leaves nothing pending */

size_t uev_uptime(char *b, size_t n, int64_t uptime_ms)
{
    if (!n) return 0;
    int64_t s = uptime_ms > 0 ? uptime_ms / 1000 : 0;
    int r = s >= 3600 ? snprintf(b, n, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60))
                      : snprintf(b, n, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    if (r < 0) { b[0] = 0; return 0; }
    return (size_t)r < n ? (size_t)r : n - 1;
}

void uev_copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    const unsigned char *s = (const unsigned char *)src;
    size_t k = strlen(src);
    if (k >= n) k = n - 1;
    /* the last sequence before k must be whole: a cut here, or one events.c's vsnprintf already made */
    size_t st = k;
    while (st > 0 && (s[st - 1] & 0xC0) == 0x80) st--;
    if (st > 0) {
        unsigned char lead = s[st - 1];
        size_t len = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (st - 1 + len > k) k = st - 1;
    }
    memcpy(dst, src, k);
    dst[k] = 0;
}

static bool starts(const char *t, const char *p) { return strncmp(t, p, strlen(p)) == 0; }

unsigned uev_category(const char *t)
{
    unsigned c = 0;
    if (strstr(t, "Wi-Fi") || starts(t, "Clock sync") || starts(t, "Clock-sync")) c |= UEV_CAT_WIFI;
    static const char *const DRIVE[] = {"Drive ", "End drive", "ECU ", "Engine ECU", "OBD ", "Adapter", "Polling ", "Searching for an OBD",
                                        "Search:", "Skipping ", "Safe to turn off", "Fill-up", "Fuel learning", "Custom PID", "Mode "};
    for (size_t i = 0; i < sizeof DRIVE / sizeof DRIVE[0]; i++) if (starts(t, DRIVE[i])) { c |= UEV_CAT_DRIVE; break; }
    return c;
}

int uev_order(uint8_t head, uint8_t count, int idx[EVENT_MAX])
{
    int n = count < EVENT_MAX ? count : EVENT_MAX;
    for (int i = 0; i < n; i++) idx[i] = (head % EVENT_MAX + EVENT_MAX - 1 - i) % EVENT_MAX;   /* newest at head - 1 */
    return n;
}

bool uev_in_drive(int64_t ev_up_ms, int64_t start_up_ms, int32_t dur_s)
{
    return dur_s >= 0 && ev_up_ms >= start_up_ms && ev_up_ms <= start_up_ms + (int64_t)dur_s * 1000 + UEV_DRIVE_TAIL_MS;
}

bool uev_match(const sys_event_t *e, uev_filter_t f, const uev_ctx_t *c)
{
    switch (f) {
    case UEV_WARNINGS: return e->level == EV_WARN;
    case UEV_ERRORS:   return e->level == EV_ERROR;
    case UEV_WIFI:     return (uev_category(e->text) & UEV_CAT_WIFI) != 0;
    case UEV_DRIVE:    return c && c->win_ok ? uev_in_drive(e->uptime_ms, c->win_start_ms, c->win_dur_s)
                                             : (uev_category(e->text) & UEV_CAT_DRIVE) != 0;
    default:           return true;
    }
}

int uev_rows(const sys_event_t ring[EVENT_MAX], uint8_t head, uint8_t count, uev_filter_t f, const uev_ctx_t *c,
             uev_row_t *out, int max)
{
    int idx[EVENT_MAX], n = uev_order(head, count, idx), k = 0;
    bool live_seen = false;                               /* the newest save line in the whole ring shows the state now */
    for (int i = 0; i < n && k < max; i++) {
        const sys_event_t *e = &ring[idx[i]];
        bool save = strcmp(e->text, SAVE_TEXT) == 0, live = save && !live_seen;
        if (save) live_seen = true;
        if (!uev_match(e, f, c)) continue;
        uev_row_t *r = &out[k++];
        r->ring = idx[i];
        r->age = i;
        r->save_line = save;
        uev_uptime(r->when, sizeof r->when, e->uptime_ms);
        if (live) {
            power_state_t ps = c ? c->power : POWER_SAFE;
            const char *mk = ps == POWER_SAFE ? (c && c->mark_ok ? c->mark_ok : "") : (c && c->mark_warn ? c->mark_warn : "");
            char t[UEV_LINE];
            snprintf(t, sizeof t, "%s%s%s", mk, mk[0] ? " " : "", power_state_text(ps));
            uev_copy(r->text, sizeof r->text, t);
            r->tone = ps == POWER_SAFE ? UEV_GOOD : UEV_ERROR;
        } else if (save) {
            uev_copy(r->text, sizeof r->text, "Saved");
            r->tone = UEV_MUTED;
        } else {
            uev_copy(r->text, sizeof r->text, e->text);
            r->tone = e->level == EV_ERROR ? UEV_ERROR : e->level == EV_WARN ? UEV_WARN : UEV_TEXT;
        }
        char l[2 * UEV_LINE];
        snprintf(l, sizeof l, "%s %s", r->when, r->text);
        uev_copy(r->line, sizeof r->line, l);
    }
    return k;
}

uint32_t uev_session(const char *t)
{
    static const char *const P[] = {"Drive s", "Drive log s"};
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        if (!starts(t, P[i])) continue;
        const char *d = t + strlen(P[i]);
        uint32_t v = 0;
        int nd = 0;
        while (d[nd] >= '0' && d[nd] <= '9' && nd < 9) { v = v * 10 + (uint32_t)(d[nd] - '0'); nd++; }
        if (nd == 6) return v;                            /* sNNNNNN, the session file name */
    }
    return 0;
}

const char *uev_level_name(event_level_t l)
{
    return l == EV_ERROR ? "Error" : l == EV_WARN ? "Warning" : "Information";
}

const char *uev_filter_name(uev_filter_t f)
{
    static const char *const N[UEV_FILTERS] = {"All", "Warnings", "Errors", "Wi-Fi", "Drive"};
    return f >= 0 && f < UEV_FILTERS ? N[f] : "";
}

void uev_pattern(char *b, size_t n, uint32_t windows, int16_t id, float novelty, uint8_t used)
{
    if (!n) return;
    if (windows) snprintf(b, n, "pattern %d  novelty %.1f  (%u/%lu)", id, (double)novelty, used, (unsigned long)windows);
    else snprintf(b, n, "pattern: first window pending");
}

int64_t uev_epoch_ms(int64_t ev_up_ms, int64_t now_up_ms, int64_t now_epoch_ms)
{
    return now_epoch_ms - (now_up_ms - ev_up_ms);
}

uint32_t uev_sig(const sys_event_t ring[EVENT_MAX], uint8_t head, uint8_t count, power_state_t power, uint32_t pattern_windows)
{
    uint32_t h = 2166136261u;                             /* FNV-1a over what the rows show */
#define MIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    MIX(head); MIX(count); MIX(power); MIX(pattern_windows);
    int idx[EVENT_MAX], n = uev_order(head, count, idx);
    for (int i = 0; i < n; i++) {
        const sys_event_t *e = &ring[idx[i]];
        MIX(e->uptime_ms / 1000); MIX(e->level);
        for (const char *p = e->text; *p; p++) MIX((unsigned char)*p);
    }
#undef MIX
    return h;
}

size_t uev_redact(const char *text, const char *name, const char *placeholder, char *out, size_t n)
{
    if (!out || !n) return 0;
    size_t w = 0, nl = name ? strlen(name) : 0, pl = strlen(placeholder);
    out[0] = 0;
    if (!text) return 0;
    for (const char *p = text; *p;) {
        size_t rest = strlen(p);
        bool whole = nl && rest >= nl && !memcmp(p, name, nl);
        bool tail = nl && !whole && rest < nl && rest >= 3 && !memcmp(p, name, rest);   /* the name cut by the text's size */
        if (whole || tail) {
            for (size_t i = 0; i < pl && w + 1 < n; i++) out[w++] = placeholder[i];
            p += whole ? nl : rest;
        } else {
            if (w + 1 < n) out[w++] = *p;
            p++;
        }
    }
    out[w] = 0;
    return w;
}

int uev_collect(sys_event_t *list, int n, int max, const sys_event_t ring[EVENT_MAX], uint8_t head, uint8_t count,
                int64_t from_ms, int64_t *seen_ms, const char *name)
{
    int idx[EVENT_MAX], k = uev_order(head, count, idx);
    for (int i = k - 1; i >= 0; i--) {                   /* oldest first */
        const sys_event_t *e = &ring[idx[i]];
        if (e->uptime_ms < from_ms || e->uptime_ms <= *seen_ms) continue;
        *seen_ms = e->uptime_ms;
        char t[EVENT_TEXT + 16];
        uev_redact(e->text, name, "<network>", t, sizeof t);
        if (n > 0 && !strncmp(list[n - 1].text, t, EVENT_TEXT - 1) && list[n - 1].level == e->level) {
            list[n - 1].uptime_ms = e->uptime_ms;        /* the same text again: events.c folds it, so do we */
            continue;
        }
        if (n == max) { memmove(&list[0], &list[1], (size_t)(max - 1) * sizeof list[0]); n--; }
        list[n].uptime_ms = e->uptime_ms;
        list[n].level = e->level;
        uev_copy(list[n].text, sizeof list[n].text, t);
        n++;
    }
    return n;
}

size_t uev_events_json(const sys_event_t *list, int n, char *out, size_t cap)
{
    size_t w = 0;
#define PUT(...) do { int r_ = snprintf(out + w, cap > w ? cap - w : 0, __VA_ARGS__); if (r_ < 0 || (size_t)r_ >= cap - w) goto full; w += (size_t)r_; } while (0)
    if (!out || !cap) return 0;
    PUT(",\"events\":[");
    for (int i = 0; i < n; i++) {
        const sys_event_t *e = &list[i];
        PUT("%s{\"up\":%lld,\"lv\":\"%c\",\"tx\":\"", i ? "," : "", (long long)e->uptime_ms, "IWE"[e->level <= EV_ERROR ? e->level : 0]);
        for (const unsigned char *p = (const unsigned char *)e->text; *p; p++) {
            if (*p == '"' || *p == '\\') PUT("\\%c", *p);
            else if (*p < 0x20) PUT("\\u%04x", *p);
            else PUT("%c", *p);
        }
        PUT("\"}");
    }
    PUT("]");
    return w;
full:
    out[0] = 0;
    return 0;
#undef PUT
}

static event_level_t lv_of(char c) { return c == 'E' ? EV_ERROR : c == 'W' ? EV_WARN : EV_INFO; }

int uev_events_parse(const char *json, sys_event_t *list, int max)
{
    /* the exact shape uev_events_json writes: {"up":N,"lv":"X","tx":"..."} (a '}' or ',' inside a text is fine) */
    const char *p = json ? strstr(json, "\"events\":[") : NULL;
    int n = 0;
    if (!p) return 0;
    p += 10;
    while (n < max && *p && *p != ']') {
        if (*p == ',' || *p == ' ') { p++; continue; }
        if (strncmp(p, "{\"up\":", 6)) break;
        char *e1;
        long long up = strtoll(p + 6, &e1, 10);
        if (e1 == p + 6 || strncmp(e1, ",\"lv\":\"", 7) || !e1[7] || strncmp(e1 + 8, "\",\"tx\":\"", 8)) break;
        sys_event_t e = { .uptime_ms = up, .level = lv_of(e1[7]) };
        const char *q = e1 + 16;
        size_t w = 0;
        for (; *q && *q != '"'; q++) {
            char c = *q;
            if (c == '\\' && q[1]) {
                q++;
                if (*q == 'u' && q[1] && q[2] && q[3] && q[4]) { char h[5] = {q[1], q[2], q[3], q[4], 0}; c = (char)strtol(h, NULL, 16); q += 4; }
                else c = *q;
            }
            if (w + 1 < sizeof e.text) e.text[w++] = c;  /* a text longer than an event's: the rest is skipped */
        }
        e.text[w] = 0;
        if (strncmp(q, "\"}", 2)) break;
        p = q + 2;
        list[n++] = e;
    }
    return n;
}

bool uev_sys_line(const char *line, sys_event_t *out)
{
    char *end;
    long long up = strtoll(line, &end, 10);
    if (end == line || end[0] != ',' || !end[1] || end[2] != ',') return false;
    char k = end[1];
    if (k != 'I' && k != 'W' && k != 'E') return false;  /* H = heartbeat, the header line, anything else */
    out->uptime_ms = up;
    out->level = lv_of(k);
    char t[EVENT_TEXT];
    size_t w = 0;
    for (const char *p = end + 3; *p && *p != '\n' && *p != '\r' && w + 1 < sizeof t; p++) t[w++] = *p;
    t[w] = 0;
    uev_copy(out->text, sizeof out->text, t);
    return true;
}

double uev_json_num(const char *json, const char *key, double def)
{
    char k[48];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = json ? strstr(json, k) : NULL;
    if (!p) return def;
    char *e;
    double v = strtod(p + strlen(k), &e);
    return e == p + strlen(k) ? def : v;
}

