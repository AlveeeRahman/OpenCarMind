/* CarMind R.E.V torque from physics, see rev_torque.h. */
#include "rev_torque.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

void rev_tq_init(rev_tq_t *T) { memset(T, 0, sizeof *T); }

void rev_tq_new_drive(rev_tq_t *T) { T->head = T->n = 0; }

float rev_tq_vd(const rev_phys_t *P)
{
    float k = rev_k_ready(&P->abs) ? P->abs.k : REV_PHYS_K0;
    float vd = 12000.0f * k / 1.184f;
    if (!isfinite(vd)) vd = 12000.0f * REV_PHYS_K0 / 1.184f;
    return vd < REV_TQ_VD_MIN ? REV_TQ_VD_MIN : vd > REV_TQ_VD_MAX ? REV_TQ_VD_MAX : vd;
}

float rev_tq_friction(float rpm, float map_kpa, bool map_ok, float baro_kpa, float vd_l)
{
    float n = rpm / 1000.0f;
    float rub = 97.0f + 15.0f * n + 5.0f * n * n;
    float pump = map_ok && isfinite(map_kpa) ? fmaxf(baro_kpa - map_kpa, 0.0f) : 0.0f;
    return (rub + pump) * vd_l / (4.0f * PI_F);                       /* kPa * L = J per cycle; a cycle is 4 pi rad */
}

float rev_tq_fuel(float mdot_kgps, float rpm)
{
    if (!(rpm >= REV_TQ_RPM_RUN) || !isfinite(mdot_kgps) || mdot_kgps < 0) return 0.0f;
    float w = 2.0f * PI_F * rpm / 60.0f;
    return REV_TQ_ETA_I * mdot_kgps * REV_TQ_QLHV / w;
}

rev_cut_t rev_tq_cut(const rev_tq_in_t *in)
{
    if (in->eqr_ok && isfinite(in->eqr) && in->eqr < 0.05f) return REV_CUT_KNOWN;
    if (in->rate_ok && in->rate_lph == 0.0f && in->rpm_ok && in->rpm > 900.0f) return REV_CUT_KNOWN;
    if (in->load_ok && in->rpm_ok && in->speed_ok && in->load < REV_TQ_CUT_LOAD && in->rpm > 1100.0f &&
        in->speed_kph > 10.0f && in->speed_kph <= in->prev_speed_kph) return REV_CUT_RULE;
    return REV_CUT_NO;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool rev_tq_row(rev_tq_t *T, const rev_tq_in_t *in, const rev_phys_t *P, rev_tq_row_t *out)
{
    memset(out, 0, sizeof *out);
    if (!in->rpm_ok || !isfinite(in->rpm) || in->rpm < REV_TQ_RPM_RUN || in->rpm > REV_TQ_RPM_MAX) return false;
    float vd = rev_tq_vd(P);
    float baro = in->baro_ok && isfinite(in->baro_kpa) && in->baro_kpa > 50 && in->baro_kpa < 120 ? in->baro_kpa : REV_TQ_PAMB;
    /* MAP missing: absolute load is the cylinder fill relative to standard air, so MAP is about p_amb * AL / 100 */
    bool map_ok = in->map_ok && isfinite(in->map_kpa);
    float map = map_ok ? in->map_kpa : 0.0f;
    if (!map_ok && in->abs_ok && isfinite(in->abs_load)) { map = baro * in->abs_load / 100.0f; map_ok = true; }
    out->t_fric = rev_tq_friction(in->rpm, map, map_ok, baro, vd);
    out->cut = rev_tq_cut(in);

    bool pid = in->tq_ok && in->ref_ok && isfinite(in->tq_pct) && isfinite(in->ref_nm) && in->ref_nm >= 20 && in->ref_nm <= 5000;
    if (pid && out->cut == REV_CUT_KNOWN && T->pid_conv == 0) {
        T->pid_cut_n++;
        if (in->tq_pct < -5.0f) T->pid_cut_neg++;
        if (T->pid_cut_n >= REV_TQ_CONV_ROWS) T->pid_conv = 2 * T->pid_cut_neg > T->pid_cut_n ? 2 : 1;
    }
    if (pid && T->pid_conv) {
        float t = in->tq_pct * in->ref_nm / 100.0f;
        out->t_b = T->pid_conv == 2 ? t : t - out->t_fric;
        out->t_fuel = T->pid_conv == 2 ? out->t_b + out->t_fric : t;
        out->src = REV_TQ_PID;
    } else if (in->petrol) {
        float mdot = -1;
        if (in->rate_ok && isfinite(in->rate_lph) && in->rate_lph >= 0 && in->rate_lph < 100) {
            mdot = in->rate_lph * REV_TQ_RHO_F / 3600.0f; out->src = REV_TQ_RATE;
        } else if (in->maf_ok && isfinite(in->maf_gps) && in->maf_gps >= 0 && in->maf_gps < 655.0f) {
            mdot = in->maf_gps / 1000.0f / REV_TQ_AFR; out->src = REV_TQ_MAF;
        } else if (in->abs_ok && isfinite(in->abs_load) && in->abs_load >= 0) {
            float k = rev_k_ready(&P->abs) ? P->abs.k : REV_PHYS_K0;
            mdot = k * in->abs_load * in->rpm / 1000.0f / REV_TQ_AFR; out->src = REV_TQ_K;
        } else return false;
        if (out->cut != REV_CUT_NO) mdot = 0.0f;
        out->t_fuel = rev_tq_fuel(mdot, in->rpm);
        out->t_b = out->t_fuel - out->t_fric;
    } else return false;
    out->t_b = clampf(out->t_b, -40.0f * vd, 200.0f * vd);
    if (!isfinite(out->t_b)) return false;
    T->rows[out->src]++;
    return true;
}

void rev_tq_push(rev_tq_t *T, int64_t t_ms, const rev_tq_row_t *row, float kph)
{
    if (T->n) {
        int last = (T->head + REV_TQ_RING - 1) % REV_TQ_RING;
        if (t_ms - T->t_ms[last] > 10000) T->n = 0;                    /* a long gap starts a new window */
    }
    T->t_ms[T->head] = t_ms;
    T->t_b[T->head] = row->t_b;
    T->kph[T->head] = isfinite(kph) && kph >= 0 ? kph : 0.0f;
    T->src[T->head] = (uint8_t)row->src;
    T->head = (T->head + 1) % REV_TQ_RING;
    if (T->n < REV_TQ_RING) T->n++;
}

bool rev_tq_mean(const rev_tq_t *T, int64_t end_ms, float *t_b, rev_tq_src_t *src, bool *show)
{
    float s = 0, v = 0;
    int n = 0;
    int64_t first = end_ms, last = 0;
    uint8_t latest = REV_TQ_NONE;
    for (int i = 0; i < T->n; i++) {
        int j = (T->head + REV_TQ_RING - 1 - i) % REV_TQ_RING;
        int64_t t = T->t_ms[j];
        if (t > end_ms || t <= end_ms - REV_TQ_SPAN_MS) continue;
        if (!n) latest = T->src[j];
        s += T->t_b[j]; v += T->kph[j]; n++;
        if (t < first) first = t;
        if (t > last) last = t;
    }
    if (n < 2 || last - first < 3000) return false;
    *t_b = s / (float)n;
    *src = (rev_tq_src_t)latest;
    *show = v / (float)n >= 2.0f;
    return true;
}

const char *rev_tq_src_name(rev_tq_src_t s)
{
    static const char *n[] = {"none", "pid", "rate", "maf", "k"};
    return (unsigned)s < 5 ? n[s] : "none";
}
