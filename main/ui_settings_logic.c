#include "ui_settings_logic.h"
#include <string.h>

int ocm_settings_migrate(const ui_settings_t *saved, ui_settings_t *out, int napps, int wifi)
{
    if (napps < 1 || napps > UI_MAX_APPS || saved->n > UI_MAX_APPS) return OCM_SET_REJECT;
    if (saved->ver != 5 && saved->ver != UI_SETTINGS_VER) return OCM_SET_REJECT;
    ui_settings_t m = *saved;
    if (saved->ver == 5) {
        /* the version-5 registry was this one without the Wi-Fi app: shift, then put Wi-Fi just before Settings */
        int k = 0;
        uint8_t ord[2 * UI_MAX_APPS];                  /* each entry plus at most one Wi-Fi */
        for (int i = 0; i < saved->n; i++) {
            int a = saved->order[i];
            if (wifi >= 0 && a >= wifi) a++;
            if (wifi >= 0 && a == napps - 1) ord[k++] = (uint8_t)wifi;
            ord[k++] = (uint8_t)a;
        }
        m.n = 0;
        for (int i = 0; i < k && m.n < UI_MAX_APPS; i++) m.order[m.n++] = ord[i];
        m.ver = UI_SETTINGS_VER;
    }
    m.hidden_v5 = 0;                                       /* every app a user had hidden is on Home again */
    /* keep valid, first-seen indices; apps missing from the order are appended in registry order */
    ui_settings_t v = m;
    v.n = 0;
    uint32_t seen = 0;
    for (int i = 0; i < m.n; i++) {
        int a = m.order[i];
        if (a < napps && !(seen & (1u << a))) { seen |= 1u << a; v.order[v.n++] = (uint8_t)a; }
    }
    for (int a = 0; a < napps; a++) if (!(seen & (1u << a))) v.order[v.n++] = (uint8_t)a;
    for (int i = v.n; i < UI_MAX_APPS; i++) v.order[i] = 0;
    *out = v;
    ui_settings_t in = *saved;
    for (int i = in.n; i < UI_MAX_APPS; i++) in.order[i] = 0;
    return memcmp(&in, &v, sizeof v) ? OCM_SET_CHANGED : OCM_SET_OK;
}

bool ocm_order_move(ui_settings_t *s, int pos, int dir)
{
    int to = pos + dir;
    if ((dir != 1 && dir != -1) || pos < 0 || pos >= s->n || to < 0 || to >= s->n) return false;
    uint8_t t = s->order[pos];
    s->order[pos] = s->order[to];
    s->order[to] = t;
    return true;
}

unsigned ocm_settings_diff(const ui_settings_t *a, const ui_settings_t *b)
{
    unsigned c = 0;
    if (a->theme != b->theme) c |= OCM_CHG_THEME;
    if (a->n != b->n || memcmp(a->order, b->order, a->n) || a->hidden_v5 != b->hidden_v5) c |= OCM_CHG_ORDER;
    if (a->econ_l100 != b->econ_l100) c |= OCM_CHG_UNITS;
    if (a->screen_off_s != b->screen_off_s) c |= OCM_CHG_SCREEN_OFF;
    if (a->brightness != b->brightness) c |= OCM_CHG_BRIGHT;
    if (a->ver != b->ver || a->widget != b->widget) c |= OCM_CHG_OTHER;
    return c;
}

unsigned ocm_settings_actions(unsigned chg)
{
    if (!chg) return 0;
    unsigned d = OCM_DO_SAVE;
    if (chg & OCM_CHG_THEME) d |= OCM_DO_THEME;
    if (chg & OCM_CHG_ORDER) d |= OCM_DO_LAUNCHER;
    if (chg & OCM_CHG_BRIGHT) d |= OCM_DO_PANEL;
    return d;
}

void ocm_debounce_touch(ocm_debounce_t *d, int64_t now_ms, int32_t quiet_ms)
{
    if (!d->pending) { d->pending = true; d->first_ms = now_ms; }
    d->due_ms = now_ms + quiet_ms;
}

bool ocm_debounce_fire(ocm_debounce_t *d, int64_t now_ms, int32_t max_defer_ms)
{
    if (!d->pending) return false;
    if (now_ms < d->due_ms && now_ms - d->first_ms < max_defer_ms) return false;
    d->pending = false;
    return true;
}

int32_t ocm_debounce_wait_ms(const ocm_debounce_t *d, int64_t now_ms, int32_t max_defer_ms, int32_t cap_ms)
{
    if (!d->pending) return cap_ms;
    int64_t due = d->due_ms < d->first_ms + max_defer_ms ? d->due_ms : d->first_ms + max_defer_ms;
    int64_t w = due - now_ms;
    return w <= 0 ? 0 : w < cap_ms ? (int32_t)w : cap_ms;
}

void ocm_debounce_cancel(ocm_debounce_t *d) { d->pending = false; }
