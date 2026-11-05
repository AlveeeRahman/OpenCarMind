/* CarMind R.E.V tank level learner. PROPOSAL for OpenCarMind main/rev_tank.c. See rev_tank.h. Single precision only. */
#include "rev_tank.h"
#include <math.h>
#include <string.h>

static void wf_add(rev_wf_t *w, float x, float y)       /* Welford: numerically stable running means and co-moments */
{
    w->n++;
    float inv = 1.0f / (float)w->n;
    float dx = x - w->mx; w->mx += dx * inv;
    float dy = y - w->my; w->my += dy * inv;
    float rx = x - w->mx, ry = y - w->my;
    w->cxx += dx * rx; w->cxy += dx * ry; w->cyy += dy * ry;
}

void rev_tank_init(rev_tank_t *t)
{
    memset(t, 0, sizeof *t);
    t->judging = 1;
}

void rev_tank_reset(rev_tank_t *t) { rev_tank_init(t); }

static float open_band(const rev_tank_t *t) { return t->has_bx ? t->bx1 - t->bx0 : 0.0f; }

static float slope(const rev_tank_t *t)
{
    float pxx = t->pxx + t->seg.cxx;
    return pxx > 0 ? -(t->pxy + t->seg.cxy) / pxx : NAN;
}

static float capacity(const rev_tank_t *t)
{
    float b = slope(t);
    return b > 0 ? 100.0f / b : NAN;
}

static void close_segment(rev_tank_t *t)
{
    const rev_wf_t *s = &t->seg;
    if (s->n >= 3 && t->seg_l >= REV_TANK_MIN_SEG_L && s->cxx > 0) {
        float d = expf(-t->seg_l / REV_TANK_MEM_L);
        t->pxx = t->pxx * d + s->cxx; t->pxy = t->pxy * d + s->cxy; t->pyy = t->pyy * d + s->cyy;
        t->dof += s->n - 1; t->segs++;
        t->srr *= d; t->srr1 *= d;
        t->band_l = t->band_l * d + open_band(t);
    }
    memset(&t->seg, 0, sizeof t->seg);
    t->x = t->seg_l = 0; t->has_bx = 0;
}

static void sample(rev_tank_t *t, float x, float lvl)
{
    if (!(lvl >= REV_TANK_LO && lvl <= REV_TANK_HI)) return;
    rev_wf_t *s = &t->seg;
    wf_add(s, x, lvl);
    if (!t->has_bx) { t->bx0 = x; t->has_bx = 1; }
    t->bx1 = x;
    float pxx = t->pxx + s->cxx;
    if (pxx > 0 && s->n >= 3) {
        float b = -(t->pxy + s->cxy) / pxx;
        float r = lvl - s->my + b * (x - s->mx);
        t->srr += r * r;
        if (t->has_r) t->srr1 += r * t->r_prev;
        t->r_prev = r; t->has_r = 1;
    }
}

static float mean(const float *v, int n) { float s = 0; for (int i = 0; i < n; i++) s += v[i]; return s / (float)n; }

static void tail_push(rev_tank_t *t, float lvl)
{
    if (t->tail_n == REV_TANK_K) { memmove(t->tail, t->tail + 1, sizeof(float) * (REV_TANK_K - 1)); t->tail_n--; }
    t->tail[t->tail_n++] = lvl;
}

void rev_tank_restart(rev_tank_t *t)
{
    if (t->judging && t->head_n) {                 /* a leg too short to judge: its samples join the open segment */
        for (int i = 0; i < t->head_n; i++) sample(t, t->head_x[i], t->head_l[i]);
        if (!t->has_anchor) { t->prev_anchor = mean(t->head_l, t->head_n); t->has_anchor = 1; }
    }
    if (t->tail_n) { t->prev_anchor = mean(t->tail, t->tail_n); t->has_anchor = 1; }
    t->tail_n = 0; t->head_n = 0; t->judging = 1; t->has_r = 0; t->off_s = 0;
}

void rev_tank_row(rev_tank_t *t, float dt_s, float raw_lph, float kph, bool engine_running)
{
    t->running = engine_running;
    if (!engine_running) {
        if (dt_s > 0) t->off_s += dt_s;
        if (t->off_s >= REV_TANK_STOP_S && !t->judging) rev_tank_restart(t);
        return;
    }
    t->off_s = 0;
    if (!(dt_s > 0 && dt_s <= 10) || !isfinite(raw_lph) || raw_lph < 0) return;
    float dl = raw_lph * dt_s / 3600.0f;
    t->x += dl; t->seg_l += dl;
    if (t->on_top) t->top_run_l += dl;
    float km = isfinite(kph) && kph >= 0 ? kph * dt_s / 3600.0f : 0.0f;
    float d = km > 0 ? expf(-km / REV_TANK_KM_MEM) : 1.0f;
    t->cons_l = t->cons_l * d + dl; t->cons_km = t->cons_km * d + km;
}

static void suggest(rev_tank_t *t, uint32_t session, float before, float after)
{
    float C = capacity(t);
    bool top = after >= REV_TANK_TOP;
    rev_tank_fill_t f = {++t->sug_seq, session, before, after,
                         isfinite(C) ? (after - before) * C / 100.0f + (top && t->top_n ? t->top_l : 0.0f) : NAN, 0, top};
    rev_tank_out_t o; rev_tank_get(t, &o); f.ready = o.ready;
    if (t->sug_n == REV_TANK_SUGGEST) { memmove(t->sug, t->sug + 1, sizeof t->sug[0] * (REV_TANK_SUGGEST - 1)); t->sug_n--; }
    t->sug[t->sug_n++] = f;
    t->fills++;
}

void rev_tank_level(rev_tank_t *t, uint32_t session, float lvl)
{
    if (!t->running || !isfinite(lvl) || lvl < 0 || lvl > 100.5f) return;
    if (t->on_top && lvl < REV_TANK_TOP && !t->judging) {     /* left the top stop: learn the litres it hid */
        float C = capacity(t);
        float v = t->top_run_l - (isfinite(C) ? (100.0f - lvl) * C / 100.0f : 0.0f);   /* minus the litres from 100 % to here */
        if (v < 0) v = 0;
        uint32_t k = t->top_n < 3 ? t->top_n : 3;              /* running mean of the first 4, then 1/4 per full fill */
        t->top_l = t->top_n ? t->top_l + (v - t->top_l) / (float)(k + 1) : v;
        t->top_n++; t->on_top = 0;
    }
    if (!t->judging) { sample(t, t->x, lvl); tail_push(t, lvl); return; }
    t->head_x[t->head_n] = t->x; t->head_l[t->head_n] = lvl; t->head_n++;
    if (t->head_n < REV_TANK_K) return;
    float after = mean(t->head_l, t->head_n);
    if (t->has_anchor) {
        float jump = after - t->prev_anchor;
        float s = t->n_jump >= 5 ? REV_TANK_SIGMAS * sqrtf(t->jump_ms) : 0.0f;
        float tf = s > REV_TANK_FILL ? s : REV_TANK_FILL, tb = s > REV_TANK_BREAK ? s : REV_TANK_BREAK;
        bool fill = jump >= tf, brk = !fill && -jump >= tb;
        if (!fill && !brk) {
            t->n_jump++;
            t->jump_ms += (jump * jump - t->jump_ms) / (float)(t->n_jump < 20 ? t->n_jump : 20);
        }
        if (fill) {
            suggest(t, session, t->prev_anchor, after);
            if (after >= REV_TANK_TOP) { t->on_top = 1; t->top_run_l = t->x - t->head_x[0]; }
        }
        if (brk) t->breaks++;
        if (fill || brk) {
            float x0 = t->head_x[0];
            close_segment(t);
            for (int i = 0; i < t->head_n; i++) t->head_x[i] -= x0;
            t->x = t->seg_l = t->head_x[t->head_n - 1];
        }
    } else if (after >= REV_TANK_TOP) {
        t->on_top = 1; t->top_run_l = 0;                       /* first anchor ever: on the top stop after an unseen fill */
    }
    for (int i = 0; i < t->head_n; i++) sample(t, t->head_x[i], t->head_l[i]);
    t->tail_n = 0;
    for (int i = 0; i < t->head_n; i++) tail_push(t, t->head_l[i]);
    t->head_n = 0; t->judging = 0;
}

void rev_tank_get(const rev_tank_t *t, rev_tank_out_t *o)
{
    memset(o, 0, sizeof *o);
    o->range_km = -1; o->left_raw_l = -1; o->level = -1;
    float b = slope(t), C = capacity(t);
    float pxx = t->pxx + t->seg.cxx, pxy = t->pxy + t->seg.cxy, pyy = t->pyy + t->seg.cyy;
    uint32_t dof = t->dof + (t->seg.n ? t->seg.n - 1 : 0);
    o->band_l = t->band_l + open_band(t);
    if (t->seg.n >= 3 && isfinite(b)) o->level = t->seg.my - b * (t->x - t->seg.mx);   /* de-sloshed: the segment's line, now */
    else if (t->tail_n) o->level = t->tail[t->tail_n - 1];
    if (!isfinite(C)) return;
    o->have = true;
    o->cap_raw_l = C;
    o->span_pct = 100.0f * o->band_l / C;
    float rho = t->srr > 0 ? t->srr1 / t->srr : 0.0f;
    rho = rho < 0 ? 0 : rho > 0.98f ? 0.98f : rho;
    float sse = pyy - pxy * pxy / pxx;
    o->rel_se = dof >= 3 ? sqrtf((sse > 0 ? sse : 0) / (float)(dof - 1) / pxx) / b * sqrtf((1 + rho) / (1 - rho)) : INFINITY;
    o->ready = o->band_l >= REV_TANK_READY_L && o->span_pct >= REV_TANK_READY_SPAN && o->rel_se <= REV_TANK_READY_SE;
    if (o->level >= 0) {
        o->left_raw_l = o->level * C / 100.0f;
        if (t->on_top && t->top_n && t->top_l > t->top_run_l) o->left_raw_l += t->top_l - t->top_run_l;
        if (t->cons_km >= 5 && t->cons_l > 0) o->range_km = o->left_raw_l * t->cons_km / t->cons_l;
    }
}

void rev_tank_ack(rev_tank_t *t, uint32_t seq)
{
    int k = 0;
    for (int i = 0; i < t->sug_n; i++) if (t->sug[i].seq > seq) t->sug[k++] = t->sug[i];
    t->sug_n = (uint8_t)k;
}

bool rev_tank_valid(const rev_tank_t *t)
{
    const float v[] = {t->pxx, t->pxy, t->pyy, t->band_l, t->srr, t->srr1, t->seg.mx, t->seg.my, t->seg.cxx, t->seg.cxy, t->seg.cyy,
                       t->x, t->seg_l, t->bx0, t->bx1, t->prev_anchor, t->top_l, t->top_run_l, t->cons_l, t->cons_km, t->jump_ms};
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) if (!isfinite(v[i])) return false;
    return t->pxx >= 0 && t->band_l >= 0 && t->tail_n <= REV_TANK_K && t->sug_n <= REV_TANK_SUGGEST;
}
