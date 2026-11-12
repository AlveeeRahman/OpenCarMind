/* ids_core: see ids_core.h */
#include "ids_core.h"
#include <string.h>

void ids_init(ids_t *g)
{
    memset(g, 0, sizeof *g);
    ids_rot_init(&g->scan);
    ids_rot_init_static_only(&g->wifi);
    g->link.mad = IDS_LINK_FLOOR;
    g->state = IDS_ST_LEARNING;
}

bool ids_shadow(const ids_t *g) { return g->win < IDS_SHADOW_WIN; }

int32_t ids_link_score(const ids_link_t *l, int32_t x)
{
    if (!l->init || l->accepted < IDS_LINK_WARM) return 0;
    int64_t num = (int64_t)(x - l->med) * 10000 * 256;
    int64_t den = (int64_t)IDS_LINK_Z * ((int64_t)14826 * l->mad + (int64_t)10000 * IDS_LINK_FLOOR);
    int64_t q = num / den;
    return q < 0 ? 0 : (q > INT32_MAX ? INT32_MAX : (int32_t)q);
}

/* one window of link data: learn (frugal median/MAD, as ids_model.h), persistence 3 of 6, the note ends after
   IDS_LINK_CLEAN clean windows in a row. True once when the note starts; *ended true once when it ends. */
bool ids_link_step(ids_link_t *l, int32_t x, bool *ended)
{
    *ended = false;
    int32_t q = ids_link_score(l, x);
    bool exc = q > 256;
    l->windows++;
    l->hist = (uint8_t)(((l->hist << 1) | (exc ? 1u : 0u)) & ((1u << IDS_ROT_PERSIST_N) - 1u));
    int ones = 0;
    for (uint8_t h = l->hist; h; h &= (uint8_t)(h - 1)) ones++;
    bool start = false;
    if (exc) { l->last_exc = l->windows; l->clean = 0; } else l->clean++;
    if (!l->flag && ones >= IDS_ROT_PERSIST_K) { l->flag = true; start = true; }
    else if (l->flag && l->clean >= IDS_LINK_CLEAN) { l->flag = false; *ended = true; }
    if (!exc && !l->flag) {                                  /* learn from clean windows only (poisoning guard) */
        if (!l->init) { l->med = x; l->init = true; }
        else {
            int32_t base = l->mad > IDS_LINK_FLOOR ? l->mad : IDS_LINK_FLOOR;
            int32_t step = base >> (l->accepted >= IDS_LINK_WARM ? IDS_ROT_LR_SHIFT : IDS_ROT_WARM_SHIFT);
            if (step < 1) step = 1;
            l->med += step * ids_rot_sgn(x - l->med);
            l->mad += step * ids_rot_sgn(ids_rot_abs(x - l->med) - l->mad);
            if (l->mad < 1) l->mad = 1;
        }
        l->accepted++;
    }
    return start;
}

uint32_t ids_step(ids_t *g, const ids_in_t *in)
{
    uint32_t ch = 0;
    const bool was_shadow = ids_shadow(g), was_acting = g->acting, was_would = g->would;
    g->win += in->nwin ? in->nwin : 1;
    if (in->day != g->day) { g->day = in->day; g->blocks_today = 0; }

    /* scan head */
    g->on[IDS_H_SCAN] = in->scan.on;
    if (in->scan.on) {
        ids_rot_out_t o = ids_rot_step(&g->scan, in->scan.n_adv, in->scan.set_bits, in->scan.new_bits, in->scan.su_new);
        g->q8[IDS_H_SCAN] = o.score_q8;
        if (o.exceed) g->scan_exc_win = g->win;
        g->scan_block = o.block;
        g->scan_windows++;
        g->seen = true;
    }

    /* the hold is 10 min of wall time also when the scan stopped (the car link is up) or runs slowed */
    if (g->scan_block && g->win - g->scan_exc_win > IDS_HOLD_WIN) { ids_rot_clear(&g->scan); g->scan_block = false; }

    /* Wi-Fi head: each finished scan is one window; the persistence starts again with every round */
    if (in->round_start) g->wifi.hist = 0;
    for (int k = 0; k < in->n_wifi && k < 2; k++) {
        const ids_win_t *w = &in->wifi[k];
        ids_rot_out_t o = ids_rot_step(&g->wifi, w->n_adv, w->set_bits, w->new_bits, w->su_new);
        g->q8[IDS_H_WIFI] = o.score_q8;
        g->on[IDS_H_WIFI] = true;
        g->wifi_scans++;
        g->seen = true;
        if (o.exceed && g->wifi_block) g->wifi_until = g->win + IDS_HOLD_WIN;     /* still anomalous: the hold restarts */
        if (o.onset) {
            g->wifi_until = g->win + IDS_HOLD_WIN;
            g->wifi_reason = IDS_R_WIFI_ROT;
            ids_rot_clear(&g->wifi);                                            /* the hold is in time, not in scans */
        }
    }
    if (ids_deauth_rule(in->deauth_round)) {
        if (g->win >= g->wifi_until || g->wifi_reason != IDS_R_DEAUTH) g->wifi_reason = IDS_R_DEAUTH;
        g->wifi_until = g->win + IDS_HOLD_WIN;
        g->on[IDS_H_WIFI] = true;
        g->seen = true;
    }
    g->wifi_block = g->win < g->wifi_until;

    /* link head (advisory) */
    if (in->link_n >= IDS_LINK_MIN_N) {
        int32_t x = (int32_t)(in->link_sum16 / in->link_n);
        bool ended;
        g->q8[IDS_H_LINK] = ids_link_score(&g->link, x);
        g->on[IDS_H_LINK] = true;
        g->seen = true;
        if (ids_link_step(&g->link, x, &ended) && !ids_shadow(g)) ch |= IDS_CH_LINK_ON;
        if (ended && !ids_shadow(g)) ch |= IDS_CH_LINK_OFF;
    } else g->on[IDS_H_LINK] = false;

    /* the user's Clear: both blocks end now, the baselines stay */
    if (in->clear && (g->scan_block || g->wifi_block)) {
        ids_rot_clear(&g->scan);
        g->scan_block = false;
        g->wifi_until = g->win;
        g->wifi_block = false;
        if (was_acting) ch |= IDS_CH_CLEARED;
    }

    const bool head_block = g->scan_block || g->wifi_block;
    g->acting = head_block && !ids_shadow(g) && in->blocking_on;
    g->would = head_block && !g->acting;
    if (g->acting) g->reason = g->scan_block ? IDS_R_SCAN : g->wifi_reason;
    else if (g->would) g->reason = g->scan_block ? IDS_R_SCAN : g->wifi_reason;
    else g->reason = IDS_R_NONE;

    if (was_shadow && !ids_shadow(g)) ch |= IDS_CH_SHADOW_END;
    if (g->acting && !was_acting) { ch |= IDS_CH_BLOCK_ON; g->blocks_today++; }
    if (!g->acting && was_acting && !(ch & IDS_CH_CLEARED)) ch |= IDS_CH_BLOCK_OFF;
    if (g->would && !was_would) { ch |= IDS_CH_WOULD; g->would_n++; }

    g->state = g->acting ? IDS_ST_BLOCKING : !g->seen ? IDS_ST_LEARNING : ids_shadow(g) ? IDS_ST_SHADOW : IDS_ST_WATCHING;
    return ch;
}

uint32_t ids_block_left_s(const ids_t *g)
{
    uint32_t w = 0;
    if (g->scan_block) {
        uint32_t since = g->win - g->scan_exc_win;
        w = since < IDS_HOLD_WIN ? IDS_HOLD_WIN - since : 0;
    }
    if (g->wifi_block && g->wifi_until - g->win > w) w = g->wifi_until - g->win;
    return w * (IDS_WIN_MS / 1000u);
}

const char *ids_reason_text(uint8_t r)
{
    static const char *const T[4] = { "", "radio flood nearby", "Wi-Fi beacon flood", "Wi-Fi deauth attack" };
    return r < 4 ? T[r] : "";
}

const char *ids_state_name(uint8_t st)
{
    static const char *const T[5] = { "off", "learning", "shadow", "watching", "blocking" };
    return st < 5 ? T[st] : "?";
}
