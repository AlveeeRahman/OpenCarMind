#include "ui_events_logic.h"
#include <stdio.h>
#include <string.h>

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
