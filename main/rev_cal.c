#include "rev_cal.h"
#include <math.h>
#include <string.h>

void rev_cal_init(rev_cal_t *c)
{
    memset(c, 0, sizeof *c);
    c->c = 1.0f;
}

void rev_cal_reset(rev_cal_t *c)
{
    c->n_fills = 0;
    c->fill_rev = 0;
    c->n_iv = c->used = c->skipped = 0;
    c->c = 1.0f;
}

static rev_ledger_t *entry(rev_cal_t *c, uint32_t session)
{
    if (c->ledger_n && c->ledger[c->ledger_n - 1].session == session) return &c->ledger[c->ledger_n - 1];
    if (c->ledger_n && session < c->ledger[c->ledger_n - 1].session) return NULL;   /* ids only grow: a stale row */
    if (c->ledger_n == REV_CAL_SESSIONS) {
        c->dropped_max = c->ledger[0].session;
        memmove(&c->ledger[0], &c->ledger[1], sizeof c->ledger[0] * (REV_CAL_SESSIONS - 1));
        c->ledger_n--;
    }
    rev_ledger_t *e = &c->ledger[c->ledger_n++];
    memset(e, 0, sizeof *e);
    e->session = session;
    return e;
}

void rev_cal_row(rev_cal_t *c, uint32_t session, float dt_s, float raw_lph, float kph, bool engine_running)
{
    if (!(dt_s > 0) || dt_s > 10) return;
    rev_ledger_t *e = entry(c, session);
    if (!e) return;
    bool fuel = isfinite(raw_lph) && raw_lph >= 0;
    bool speed = isfinite(kph) && kph >= 0;
    if (speed) e->km += kph * dt_s / 3600.0f;
    if (fuel) {
        e->raw_l += raw_lph * dt_s / 3600.0f;
        e->cov_s += dt_s;
        c->life_l += (double)c->c * raw_lph * dt_s / 3600.0;
        if (speed) c->life_km += (double)kph * dt_s / 3600.0;
    } else if (engine_running) {
        e->unc_s += dt_s;
    }
}

/* Sums over the ledger drives in [from, to]. Returns false if any of them may have fallen out of the ledger. */
static bool span(const rev_cal_t *c, uint32_t from, uint32_t to, double *raw, double *km, double *cov, double *unc)
{
    *raw = *km = *cov = *unc = 0;
    if (c->dropped_max >= from) return false;
    for (int i = 0; i < c->ledger_n; i++) {
        const rev_ledger_t *e = &c->ledger[i];
        if (e->session < from || e->session > to) continue;
        *raw += e->raw_l; *km += e->km; *cov += e->cov_s; *unc += e->unc_s;
    }
    return true;
}

static void sort_fills(rev_fill_t *f, int n)                   /* by drive, then by id: deterministic for any input order */
{
    for (int i = 1; i < n; i++) {
        rev_fill_t v = f[i];
        int j = i - 1;
        while (j >= 0 && (f[j].after_session > v.after_session || (f[j].after_session == v.after_session && f[j].id > v.id))) { f[j + 1] = f[j]; j--; }
        f[j + 1] = v;
    }
}

static uint8_t judge(double pumped, double raw, double cov, double unc, bool whole)
{
    if (!whole) return REV_IV_GONE;
    if (raw < REV_CAL_MIN_RAW_L) return REV_IV_SMALL;
    if (cov + unc <= 0 || cov / (cov + unc) < REV_CAL_COVERAGE) return REV_IV_COVERAGE;
    double r = pumped / raw;
    if (r < 0.5 || r > 2.0) return REV_IV_RATIO;
    return REV_IV_USED;
}

float rev_cal_compute(rev_cal_t *c)
{
    c->n_iv = c->used = c->skipped = 0;
    bool have_start = false;
    uint32_t start = 0;
    double pumped = 0;
    for (int i = 0; i < c->n_fills; i++) {
        const rev_fill_t *f = &c->fills[i];
        rev_interval_t *iv = &c->iv[c->n_iv];
        double raw, km, cov, unc;
        if (f->kind == REV_FILL_TRIP) {
            bool whole = span(c, f->from_session, f->after_session, &raw, &km, &cov, &unc);
            *iv = (rev_interval_t){f->id, f->from_session, f->after_session, (float)(f->l_per_100km * km / 100.0), (float)raw, 0, 1};
            iv->why = judge(iv->pumped_l, raw, cov, unc, whole);
            c->n_iv++;
            continue;
        }
        if (!f->full) { if (have_start) pumped += f->litres; continue; }
        if (have_start) {
            pumped += f->litres;
            bool whole = span(c, start + 1, f->after_session, &raw, &km, &cov, &unc);
            *iv = (rev_interval_t){f->id, start + 1, f->after_session, (float)pumped, (float)raw, 0, 0};
            iv->why = judge(pumped, raw, cov, unc, whole);
            c->n_iv++;
        }
        have_start = true;
        start = f->after_session;
        pumped = 0;
    }
    /* newest interval weighs 1, the one before g, then g^2, ...; trip-computer figures count half */
    double num = 0, den = 0, g = 1;
    for (int i = c->n_iv - 1; i >= 0; i--) {
        const rev_interval_t *iv = &c->iv[i];
        if (iv->why != REV_IV_USED) { c->skipped++; continue; }
        double w = g * (iv->trip ? 0.5 : 1.0);
        num += w * iv->pumped_l;
        den += w * iv->raw_l;
        g *= REV_CAL_GAMMA;
        c->used++;
    }
    float k = den > 0 ? (float)(num / den) : 1.0f;
    c->c = k < REV_CAL_MIN ? REV_CAL_MIN : k > REV_CAL_MAX ? REV_CAL_MAX : k;
    return c->c;
}

bool rev_cal_set_fills(rev_cal_t *c, const rev_fill_t *fills, int n, uint32_t fill_rev)
{
    if (n < 0 || n > REV_CAL_FILLUPS) return false;
    for (int i = 0; i < n; i++) {
        const rev_fill_t *f = &fills[i];
        if (f->kind == REV_FILL_PUMP && !(f->litres > 0 && f->litres <= 300)) return false;
        if (f->kind == REV_FILL_TRIP && (!(f->l_per_100km >= 1 && f->l_per_100km <= 60) || f->from_session > f->after_session)) return false;
        if (f->kind > REV_FILL_TRIP) return false;
    }
    memcpy(c->fills, fills, sizeof *fills * (size_t)n);
    c->n_fills = n;
    c->fill_rev = fill_rev;
    sort_fills(c->fills, n);
    rev_cal_compute(c);
    return true;
}

const char *rev_iv_why_name(int why)
{
    static const char *const names[] = {"used", "under 5 L", "coverage under 95 %", "drives no longer on the board", "ratio outside 0.5 to 2"};
    return why >= 0 && why <= REV_IV_RATIO ? names[why] : "?";
}
