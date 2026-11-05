#include "econ.h"
#include <string.h>
#include <ctype.h>

#define KM_PER_MILE     1.609344
#define L_PER_US_GAL    3.785411784
#define L_PER_UK_GAL    4.54609

static bool cc_is(const char *cc, const char *code)
{
    return toupper((unsigned char)cc[0]) == code[0] && toupper((unsigned char)cc[1]) == code[1];
}

int units_from_country(const char *cc)
{
    if (!cc || !isalpha((unsigned char)cc[0]) || !isalpha((unsigned char)cc[1])) return -1;   /* "01" = world, unknown */
    static const char *const US[] = {"US", "PR", "GU", "VI", "AS", "MP"};                    /* US states and territories */
    for (size_t i = 0; i < sizeof US / sizeof *US; i++) if (cc_is(cc, US[i])) return UNITS_US;
    if (cc_is(cc, "GB")) return UNITS_UK;
    return UNITS_METRIC;
}

int units_from_timezone(const char *tz)
{
    if (!tz || !tz[0]) return -1;
    if (strncmp(tz, "GMT0BST", 7) == 0) return UNITS_UK;
    return -1;
}

int units_parse(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "metric")) return UNITS_METRIC;
    if (!strcmp(s, "us")) return UNITS_US;
    if (!strcmp(s, "uk")) return UNITS_UK;
    return -1;
}

const char *units_name(units_t u) { return u == UNITS_US ? "us" : u == UNITS_UK ? "uk" : "metric"; }
const char *units_econ_label(units_t u) { return u == UNITS_METRIC ? "km/L" : "mpg"; }
const char *units_dist_label(units_t u) { return u == UNITS_METRIC ? "km" : "mi"; }

const char *fuel_src_name(fuel_src_t s)
{
    return s == FUEL_PID ? "pid" : s == FUEL_MAF ? "maf" : s == FUEL_MODEL ? "model" : s == FUEL_PHYS ? "phys" : s == FUEL_LEARNED ? "learned" : "";
}

bool econ_src_is_estimate(fuel_src_t s) { return s == FUEL_MODEL || s == FUEL_PHYS || s == FUEL_LEARNED; }

static bool maf_valid(bool ok, float g) { return ok && g >= 0 && g < 655.0f; }   /* 0xFFFF (655.35 g/s) is a sensor fault, not air */
static bool est_valid(bool ok, float lph) { return ok && lph >= 0 && lph < 100; }

float econ_fuel_pick(const econ_in_t *in, fuel_src_t *src)
{
    fuel_src_t dummy;
    if (!src) src = &dummy;
    if (in->rate_ok && in->rate_lph >= 0 && in->rate_lph < 3213) { *src = FUEL_PID; return in->rate_lph; }
    if (maf_valid(in->maf_ok, in->maf_gps)) { *src = FUEL_MAF; return econ_maf_lph(in->maf_gps, in->lambda_ok, in->lambda, in->fuel_type); }
    float phys = maf_valid(in->phys_ok, in->phys_maf_gps) ? econ_maf_lph(in->phys_maf_gps, in->lambda_ok, in->lambda, in->fuel_type) : -1;
    bool phys_ok = est_valid(phys >= 0, phys);
    bool learned_ok = est_valid(in->learned_ok, in->learned_lph), model_ok = est_valid(in->model_ok, in->model_lph);
    if (in->prefer == FUEL_PHYS && phys_ok) { *src = FUEL_PHYS; return phys; }
    if (in->prefer == FUEL_LEARNED && learned_ok) { *src = FUEL_LEARNED; return in->learned_lph; }
    if (in->prefer == FUEL_MODEL && model_ok) { *src = FUEL_MODEL; return in->model_lph; }
    if (phys_ok) { *src = FUEL_PHYS; return phys; }
    if (learned_ok) { *src = FUEL_LEARNED; return in->learned_lph; }
    if (model_ok) { *src = FUEL_MODEL; return in->model_lph; }
    *src = FUEL_NONE;
    return -1;
}

float econ_fuel_lph(bool rate_ok, float rate_lph, bool maf_ok, float maf_gps, bool lambda_ok, float lambda, int fuel_type,
                    bool model_ok, float model_lph, fuel_src_t *src)
{
    econ_in_t in = {rate_ok, rate_lph, maf_ok, maf_gps, lambda_ok, lambda, fuel_type, model_ok, model_lph,
                    false, 0, false, 0, FUEL_MODEL};
    return econ_fuel_pick(&in, src);
}

float econ_maf_lph(float maf_gps, bool lambda_ok, float lambda, int fuel_type)
{
    bool diesel = fuel_type == 4 || fuel_type == 19 || fuel_type == 23;     /* PID 0x51: diesel, hybrid diesel, bifuel diesel */
    float afr = diesel ? 14.5f : 14.7f;                                      /* stoichiometric air-fuel ratio by mass */
    float density = diesel ? 832.0f : 745.0f;                                /* g/L */
    float lam = 1.0f;
    if (lambda_ok) {
        if (lambda <= 0.05f) return 0;                                       /* commanded 0: decel fuel cut */
        if (lambda >= 0.5f && lambda <= (diesel ? 8.0f : 2.0f)) lam = lambda;   /* diesels run lean: 3 to 6 at light load */
    }
    return maf_gps / (afr * lam) * 3600.0f / density;
}

void econ_reset(econ_trip_t *t) { t->km = 0; t->litres = 0; }

void econ_step(econ_trip_t *t, float kph, float lph, float dt_s)
{
    if (lph < 0 || kph < 0 || dt_s <= 0 || dt_s > 10) return;               /* unknown fuel or a gap: neither side counts */
    t->km += kph * dt_s / 3600.0;
    t->litres += lph * dt_s / 3600.0;
}

float econ_trip_kpl(const econ_trip_t *t)
{
    if (t->km < 0.5 || t->litres < 0.02) return -1;
    return (float)(t->km / t->litres);
}

float econ_instant_kpl(float kph, float lph)
{
    if (kph < 5 || lph < 0) return -1;
    if (lph < 0.05f) return 99.9f;                                           /* coasting on fuel cut: show the top of the scale */
    float k = kph / lph;
    return k > 99.9f ? 99.9f : k;
}

float econ_to_unit(float kpl, units_t u)
{
    if (kpl < 0) return kpl;
    if (u == UNITS_US) return (float)(kpl * L_PER_US_GAL / KM_PER_MILE);
    if (u == UNITS_UK) return (float)(kpl * L_PER_UK_GAL / KM_PER_MILE);
    return kpl;
}

double econ_dist_to_unit(double km, units_t u) { return u == UNITS_METRIC ? km : km / KM_PER_MILE; }
