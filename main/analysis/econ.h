/* Fuel use, fuel economy and display units. Pure C, no ESP-IDF, so the host tests cover it.
 *
 * Fuel rate comes from PID 0x5E when the car reports it. Otherwise it is worked out from the mass air flow (PID 0x10):
 *   fuel (g/s) = MAF / (AFR_stoich * lambda),   L/h = g/s * 3600 / density
 * with lambda the commanded equivalence ratio (PID 0x44) when present. A commanded ratio of 0 is decel fuel cut (no fuel).
 * Economy is distance over fuel: km/L, mpg (US gallon) or mpg (imperial gallon). */
#pragma once
#include <stdbool.h>

typedef enum { UNITS_METRIC = 0, UNITS_US = 1, UNITS_UK = 2 } units_t;

/* Units from a two-letter country code (as broadcast by a Wi-Fi access point). -1 when the code says nothing. */
int  units_from_country(const char *cc);
/* Units from a POSIX TZ string. Only the UK is unambiguous (North American zones are shared with Canada); -1 otherwise. */
int  units_from_timezone(const char *tz);
/* "metric", "us", "uk" -> units; "auto" or anything else -> -1. */
int  units_parse(const char *s);
const char *units_name(units_t u);
const char *units_econ_label(units_t u);    /* "km/L" or "mpg" */
const char *units_dist_label(units_t u);    /* "km" or "mi" */

/* Where a fuel rate came from. Older cars fall down the list; nothing is guessed past the estimates. FUEL_PHYS and
   FUEL_LEARNED come from CarMind R.E.V (rev_learn.h); they were appended, so the CSV values of the older ones did not change. */
typedef enum { FUEL_NONE = 0, FUEL_PID, FUEL_MAF, FUEL_MODEL, FUEL_PHYS, FUEL_LEARNED } fuel_src_t;
const char *fuel_src_name(fuel_src_t s);     /* "", "pid", "maf", "model", "phys", "learned" */
/* Model, physics or learned: a figure the board worked out, shown in amber. */
bool econ_src_is_estimate(fuel_src_t s);

/* Fuel rate in L/h from the best source available, or a negative value with FUEL_NONE. fuel_type is PID 0x51 (0 unknown).
   The caller decides whether a commanded 0 (fuel cut) is still current: PID 0x44 is polled slowly.
   model_lph is the on-board model's estimate (an estimate, not a measurement), used only when the car gives neither. */
float econ_fuel_lph(bool rate_ok, float rate_lph, bool maf_ok, float maf_gps, bool lambda_ok, float lambda, int fuel_type,
                    bool model_ok, float model_lph, fuel_src_t *src);
/* Fuel L/h from an airflow in g/s, with the commanded ratio when known (0 = decel fuel cut) and the fuel type: the MAF formula. */
float econ_maf_lph(float maf_gps, bool lambda_ok, float lambda, int fuel_type);

/* Everything econ_fuel_pick() may choose from. Car PID first, then MAF, then the estimates: `prefer` (FUEL_PHYS, FUEL_LEARNED or
   FUEL_MODEL, from R.E.V's running errors) when it is available, else physics, learned, factory model in that order. The physics
   estimate is an airflow (g/s) and goes through the MAF formula, so AFR, lambda and diesel handling stay in one place.
   With phys_ok and learned_ok false and prefer FUEL_MODEL this is exactly econ_fuel_lph(). */
typedef struct {
    bool rate_ok;    float rate_lph;
    bool maf_ok;     float maf_gps;
    bool lambda_ok;  float lambda;
    int  fuel_type;
    bool model_ok;   float model_lph;
    bool phys_ok;    float phys_maf_gps;
    bool learned_ok; float learned_lph;
    fuel_src_t prefer;
} econ_in_t;
float econ_fuel_pick(const econ_in_t *in, fuel_src_t *src);

typedef struct {
    double km;        /* distance covered while the fuel rate was known */
    double litres;    /* fuel used over that distance */
} econ_trip_t;

void  econ_reset(econ_trip_t *t);
/* One row: speed in km/h, fuel rate in L/h (negative = unknown, the row is skipped), seconds since the last row. */
void  econ_step(econ_trip_t *t, float kph, float lph, float dt_s);
/* Trip economy in km/L; negative until at least 0.5 km and 0.02 L are counted. */
float econ_trip_kpl(const econ_trip_t *t);
/* Instant economy in km/L at this row; negative when stopped, slow or unknown. */
float econ_instant_kpl(float kph, float lph);
/* km/L converted to the display unit (km/L, mpg US, mpg UK). Negative stays negative. */
float econ_to_unit(float kpl, units_t u);
/* km converted to the display distance unit. */
double econ_dist_to_unit(double km, units_t u);
