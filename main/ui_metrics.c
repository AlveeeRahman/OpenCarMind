/* ui_metrics.c: binned latency histograms, the Mann-Whitney rank test, Holm's step-down, the settle detector and the
 * per-class A/B gate. See ui_metrics.h for the definitions (LOCKED).
 *
 * 1. Histograms. Fixed bins of bin_ms (uint16 counts). A percentile is the nearest rank, ceil(pct * n / 100), read
 *    as the centre of the bin that holds it, so it is within half a bin of the exact sample percentile.
 *
 * 2. Mann-Whitney U from the bins (Mann & Whitney 1947). Over every pair (a, b) of an A sample and a B sample:
 *        U = #{a < b} + 0.5 #{a == b}          (same bin = a tie)
 *    Under "no difference" E[U] = nA nB / 2 and Var[U] = nA nB (nA + nB + 1) / 12. Ties lower the true variance, and
 *    the formula leaves them out, so the test is conservative. z = (U - E[U]) / sqrt(Var[U]) > 0 when A tends to be
 *    faster. One-sided 5 %: z > 1.645.
 *
 * 3. Holm (1979) step-down for m tests at family-wise level alpha: order the p-values p(1) <= ... <= p(m); reject
 *    H(1) .. H(k) where k is the largest i with p(j) <= alpha / (m - j + 1) for every j <= i. With one-sided z-tests
 *    p = 1 - Phi(z), so the same test reads: z's sorted high to low, the i-th (0-based) must exceed
 *    Phi^-1(1 - alpha / (m - i)). The critical values below are Phi^-1(1 - 0.05 / k) rounded up to 0.01, so an integer
 *    z x 100 (truncated) at or above them is always past the exact value.
 *
 * 4. The settle detector is a small state machine fed by LVGL's display events; see the header. */
#include "ui_metrics.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define UM_HOT IRAM_ATTR                                     /* the per-event path: no flash fetch on a cold cache */
#else
#define UM_HOT
#endif

/* ---------- histograms ---------- */
uint32_t um_hist_n(const uint16_t *h, int nb)
{
    uint32_t n = 0;
    for (int b = 0; b < nb; b++) n += h[b];
    return n;
}

uint16_t um_hist_pct(const uint16_t *h, int nb, int bin_ms, unsigned pct)
{
    uint32_t n = um_hist_n(h, nb), c = 0, rank = (pct * n + 99) / 100;
    int b = 0;
    if (!n) return 0;
    while (b < nb - 1 && (c += h[b]) < rank) b++;
    return (uint16_t)(b * bin_ms + bin_ms / 2);
}

UM_HOT int um_lat_bin(uint32_t ms)
{
    if (ms < UM_NB1 * UM_BIN_MS) return (int)(ms / UM_BIN_MS);
    uint32_t b = UM_NB1 + (ms - UM_NB1 * UM_BIN_MS) / UM_BIN2_MS;
    return (int)(b < UM_NB ? b : UM_NB - 1);
}

uint16_t um_lat_centre(int b)
{
    return (uint16_t)(b < UM_NB1 ? b * UM_BIN_MS + UM_BIN_MS / 2 : UM_NB1 * UM_BIN_MS + (b - UM_NB1) * UM_BIN2_MS + UM_BIN2_MS / 2);
}

uint16_t um_lat_pct(const uint16_t *h, unsigned pct)
{
    uint32_t n = um_hist_n(h, UM_NB), c = 0, rank = (pct * n + 99) / 100;
    int b = 0;
    if (!n) return 0;
    while (b < UM_NB - 1 && (c += h[b]) < rank) b++;
    return um_lat_centre(b);
}

int16_t um_mw_z100(const uint16_t *a, const uint16_t *b, int nb)
{
    float na = (float)um_hist_n(a, nb), nbs = (float)um_hist_n(b, nb), u = 0, above = nbs;
    if (na < 1 || nbs < 1) return 0;
    for (int i = 0; i < nb; i++) { above -= b[i]; u += (float)a[i] * (above + 0.5f * (float)b[i]); }
    float m = u - 0.5f * na * nbs, z2 = m * m / (na * nbs * (na + nbs + 1) * (1.0f / 12.0f)), g = z2 + 1;
    for (int k = 0; k < 20; k++) g = 0.5f * (g + z2 / g);      /* sqrt by Newton, no libm */
    g = g > 320 ? 320 : g;
    return (int16_t)(m < 0 ? -g * 100 : g * 100);
}

int16_t um_holm_crit100(int k)
{
    static const int16_t C[8] = { 165, 196, 213, 225, 233, 240, 245, 250 };   /* 1.6449 1.9600 2.1280 2.2414 2.3263
                                                                                  2.3940 2.4500 2.4977 */
    return C[k < 1 ? 0 : k > 8 ? 7 : k - 1];
}

void um_holm(const int16_t *z100, const uint8_t *has, int m, uint8_t *sig)
{
    uint8_t ord[8], k = 0;
    if (m > 8) m = 8;
    for (int i = 0; i < m; i++) { sig[i] = 0; if (has[i]) ord[k++] = (uint8_t)i; }
    for (int i = 1; i < k; i++)                               /* insertion sort, z high to low */
        for (int j = i; j > 0 && z100[ord[j]] > z100[ord[j - 1]]; j--) { uint8_t t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
    for (int i = 0; i < k; i++) {
        if (z100[ord[i]] < um_holm_crit100(m - i)) break;   /* the first failure stops the step-down */
        sig[ord[i]] = 1;
    }
}

/* ---------- settle detector ---------- */
UM_HOT void um_settle_lift(um_settle_t *s, uint32_t t_lift)
{
    s->phase = UM_S_WAIT;
    s->t_lift = s->t_out = t_lift;
    /* areas invalidated before the lift and not drawn yet belong to the transition too: keep s->dirty */
}

UM_HOT void um_settle_invalidated(um_settle_t *s) { s->dirty = 1; }
UM_HOT void um_settle_render_start(um_settle_t *s) { s->dirty = 0; }

UM_HOT void um_settle_frame(um_settle_t *s, uint32_t t)
{
    if (s->phase == UM_S_WAIT && (int32_t)(t - s->t_out) > 0) s->t_out = t;
}

UM_HOT int um_settle_poll(um_settle_t *s, uint32_t t, bool busy, uint32_t *lat_ms)
{
    if (s->phase != UM_S_WAIT) return UM_S_IDLE;
    if (t - s->t_lift >= UM_TIMEOUT_MS) {
        s->phase = UM_S_IDLE;
        *lat_ms = UM_TIMEOUT_MS;
        return UM_S_TIMEOUT;
    }
    if (busy || s->dirty || t - s->t_out < UM_QUIET_MS) return UM_S_WAIT;
    s->phase = UM_S_IDLE;
    *lat_ms = s->t_out - s->t_lift;
    return UM_S_SETTLED;
}

UM_HOT void um_settle_abort(um_settle_t *s) { s->phase = UM_S_IDLE; }

/* ---------- classes, stats, gate ---------- */
static inline UM_HOT uint32_t xs32(uint32_t *r) { uint32_t v = *r; v ^= v << 13; v ^= v >> 17; v ^= v << 5; return *r = v; }
static inline bool gated(const um_t *m, uint8_t c) { return c < UM_NCLASS && ((m->family >> c) & 1u); }

const char *um_class_name(uint8_t c)
{
    static const char *const N[UM_NCLASS] = { "swipe", "app", "back", "control", "list" };
    return c < UM_NCLASS ? N[c] : "other";
}

const char *um_state_name(uint8_t st)
{
    static const char *const N[4] = { "learning", "KEEP", "OFF", "warm-up" };
    return st < 4 ? N[st] : "?";
}

static void class_clear(um_t *m, int c)
{
    memset(m->hist[c], 0, sizeof m->hist[c]);
    memset(m->k[c].n, 0, sizeof m->k[c].n);
    memset(m->k[c].rdy, 0, sizeof m->k[c].rdy);
    memset(&m->k[c].v, 0, sizeof m->k[c].v);
    m->k[c].seen = 0;
    m->k[c].v.state = gated(m, (uint8_t)c) ? UM_WARMUP : UM_LEARNING;
}

void um_init(um_t *m, uint32_t seed, uint8_t family, uint32_t fw_tag)
{
    memset(m, 0, sizeof *m);
    m->family = (uint8_t)(family & ((1u << UM_NCLASS) - 1u) & ~(1u << UM_SWIPE));   /* smart swipe has its own gate */
    m->rng = seed ? seed : 0x2545F491u;
    m->fw_tag = fw_tag;
    for (int c = 0; c < UM_NCLASS; c++) class_clear(m, c);
}

void um_reset_verdicts(um_t *m)
{
    for (int c = 0; c < UM_NCLASS; c++) class_clear(m, c);
}

UM_HOT uint8_t um_arm(um_t *m, uint8_t c)
{
    if (!gated(m, c)) return UM_A0;
    switch (m->k[c].v.state) {
    case UM_KEEP:     return UM_A1;
    case UM_LEARNING: return (uint8_t)((xs32(&m->rng) >> 7) & 1u);
    default:          return UM_A0;                       /* warm-up, OFF */
    }
}

UM_HOT void um_record(um_t *m, uint8_t c, uint8_t arm, uint32_t lat_ms, bool ready, uint8_t flags)
{
    if (c >= UM_NCLASS || arm > 1) return;
    m->k[c].total++;
    if (flags & UM_F_TIMEOUT) m->k[c].timeouts++;
    if (flags & UM_F_INTERRUPTED) m->k[c].interrupted++;
    if (gated(m, c) && m->k[c].v.state == UM_WARMUP) {           /* warm-up events are not A/B samples */
        if (++m->k[c].seen >= UM_WARMUP) m->k[c].v.state = UM_LEARNING;
        return;
    }
    if (m->k[c].seen < 0xFFFF) m->k[c].seen++;
    uint16_t *h = m->hist[c][arm];
    h[um_lat_bin(lat_ms)]++;
    m->k[c].n[arm]++;
    if (ready) m->k[c].rdy[arm]++;
    if (m->k[c].n[arm] >= UM_HALVE_AT) {                         /* recency decay */
        for (int i = 0; i < UM_NB; i++) h[i] = (uint16_t)((h[i] + 1u) / 2u);
        m->k[c].n[arm] = (uint16_t)um_hist_n(h, UM_NB);
        m->k[c].rdy[arm] /= 2;
        if (m->k[c].rdy[arm] > m->k[c].n[arm]) m->k[c].rdy[arm] = m->k[c].n[arm];
    }
}

UM_HOT void um_dropped(um_t *m, uint8_t c) { if (c < UM_NCLASS) m->k[c].dropped++; }

void um_stats(const um_t *m, uint8_t c, uint8_t arm, um_stats_t *o)
{
    memset(o, 0, sizeof *o);
    if (c >= UM_NCLASS || arm > 1) return;
    o->n = m->k[c].n[arm];
    o->p50 = um_lat_pct(m->hist[c][arm], 50);
    o->p95 = um_lat_pct(m->hist[c][arm], 95);
    o->ready_pm = o->n ? (uint16_t)(1000u * m->k[c].rdy[arm] / o->n) : 0;
}

uint8_t um_decide(uint16_t n0, uint16_t n1, uint16_t p95_0, uint16_t p95_1, uint16_t ready1_pm, bool z_sig, uint8_t *reason)
{
    uint8_t r = UM_R_NONE, st = UM_LEARNING;
    if (n0 >= UM_MIN_PER_ARM && n1 >= UM_MIN_PER_ARM) {
        r = ready1_pm <= UM_READY_PM ? UM_R_READY_LOW : !(p95_1 < p95_0) ? UM_R_SLOWER : !z_sig ? UM_R_UNCERTAIN : UM_R_PASS;
        st = r == UM_R_PASS ? UM_KEEP : UM_OFF;
        if (r == UM_R_UNCERTAIN && (n0 < UM_MAX_PER_ARM || n1 < UM_MAX_PER_ARM)) st = UM_LEARNING;   /* look again later */
    }
    if (reason) *reason = r;
    return st;
}

bool um_gate_step(um_t *m)
{
    int16_t z[UM_NCLASS] = { 0 };
    uint8_t has[UM_NCLASS] = { 0 }, sig[UM_NCLASS], idx[UM_NCLASS], k = 0, live = 0;
    um_stats_t s0[UM_NCLASS], s1[UM_NCLASS];
    for (int c = 0; c < UM_NCLASS; c++) {
        if (!gated(m, (uint8_t)c)) continue;
        uint8_t i = k++;
        idx[i] = (uint8_t)c;
        if (m->k[c].v.state == UM_KEEP || m->k[c].v.state == UM_OFF) { z[i] = m->k[c].v.z100; has[i] = m->k[c].v.n0 >= UM_MIN_PER_ARM; continue; }
        if (m->k[c].v.state != UM_LEARNING || m->k[c].n[0] < UM_MIN_PER_ARM || m->k[c].n[1] < UM_MIN_PER_ARM) continue;
        um_stats(m, (uint8_t)c, 0, &s0[c]);
        um_stats(m, (uint8_t)c, 1, &s1[c]);
        z[i] = um_mw_z100(m->hist[c][1], m->hist[c][0], UM_NB);   /* > 0: A1 settles sooner */
        has[i] = 1;
        live++;
    }
    if (!live) return false;
    um_holm(z, has, k, sig);
    bool changed = false;
    for (int i = 0; i < k; i++) {
        int c = idx[i];
        if (m->k[c].v.state != UM_LEARNING || !has[i]) continue;
        uint8_t r, st = um_decide(s0[c].n, s1[c].n, s0[c].p95, s1[c].p95, s1[c].ready_pm, sig[i], &r);
        if (st == UM_LEARNING) continue;
        um_verdict_t *v = &m->k[c].v;
        v->state = st; v->reason = r; v->z100 = z[i];
        v->p95_0 = s0[c].p95; v->p95_1 = s1[c].p95; v->ready_pm = s1[c].ready_pm; v->n0 = s0[c].n; v->n1 = s1[c].n;
        changed = true;
    }
    return changed;
}

/* ---------- persistence ----------
   magic, fw_tag, family (3 x u32), the verdicts, the warm-up counters, then for each gated class its two histograms and
   n / ready counts, then FNV-1a over everything before it. Built in the caller's buffer (unaligned is fine). */
#define UM_MAGIC 0x32544D55u                                   /* "UMT2": 192 latency bins */
typedef char um_blob_check[(UM_BLOB_MAX >= 16) ? 1 : -1];

static uint32_t fnv1a(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static size_t blob_len(uint8_t family)
{
    size_t n = 12 + UM_NCLASS * (sizeof(um_verdict_t) + 2);
    for (int c = 0; c < UM_NCLASS; c++) if ((family >> c) & 1u) n += 2 * UM_NB * 2 + 8;
    return n + 4;
}

size_t um_save(const um_t *m, void *buf, size_t cap)
{
    uint8_t *b = (uint8_t *)buf;
    size_t len = blob_len(m->family), o = 12;
    uint32_t hdr[3] = { UM_MAGIC, m->fw_tag, m->family };
    if (!b || cap < len) return 0;
    memcpy(b, hdr, 12);
    for (int c = 0; c < UM_NCLASS; c++) { memcpy(b + o, &m->k[c].v, sizeof(um_verdict_t)); o += sizeof(um_verdict_t); }
    for (int c = 0; c < UM_NCLASS; c++) { memcpy(b + o, &m->k[c].seen, 2); o += 2; }
    for (int c = 0; c < UM_NCLASS; c++) {
        if (!gated(m, (uint8_t)c)) continue;
        memcpy(b + o, m->hist[c], sizeof m->hist[c]); o += sizeof m->hist[c];
        memcpy(b + o, m->k[c].n, 4); o += 4;
        memcpy(b + o, m->k[c].rdy, 4); o += 4;
    }
    uint32_t sum = fnv1a(b, o);
    memcpy(b + o, &sum, 4);
    return o + 4;
}

bool um_load(um_t *m, const void *buf, size_t n)
{
    const uint8_t *b = (const uint8_t *)buf;
    if (!b || n < 16 || rd32(b) != UM_MAGIC || rd32(b + 8) != m->family || n != blob_len(m->family) ||
        rd32(b + n - 4) != fnv1a(b, n - 4)) return false;
    if (rd32(b + 4) != m->fw_tag) return true;                 /* new firmware: valid blob, verdicts not carried */
    size_t o = 12;
    um_verdict_t v[UM_NCLASS];
    memcpy(v, b + o, sizeof v); o += sizeof v;
    for (int c = 0; c < UM_NCLASS; c++) if (v[c].state > UM_WARMUP || v[c].reason > UM_R_READY_LOW) return false;
    for (int c = 0; c < UM_NCLASS; c++) { m->k[c].v = v[c]; memcpy(&m->k[c].seen, b + o, 2); o += 2; }
    for (int c = 0; c < UM_NCLASS; c++) {
        if (!gated(m, (uint8_t)c)) { m->k[c].v.state = UM_LEARNING; continue; }
        memcpy(m->hist[c], b + o, sizeof m->hist[c]); o += sizeof m->hist[c];
        memcpy(m->k[c].n, b + o, 4); o += 4;
        memcpy(m->k[c].rdy, b + o, 4); o += 4;
        for (int a = 0; a < 2; a++) {                          /* counts must agree with the bins */
            if (m->k[c].n[a] != um_hist_n(m->hist[c][a], UM_NB) || m->k[c].rdy[a] > m->k[c].n[a]) { class_clear(m, c); break; }
        }
    }
    return true;
}
