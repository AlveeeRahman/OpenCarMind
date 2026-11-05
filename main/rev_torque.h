/* CarMind R.E.V: engine (brake) torque from physics, for cars that do not report torque. Pure C, host-tested
 * (tests/host/test_rev_learn.c).
 *
 * Brake torque = indicated torque from the fuel's energy minus friction:
 *     T_b = eta_i * mdot_f * Q_LHV / omega  -  T_f,      omega = 2 pi rpm / 60
 *     T_f = (rubbing MEP + pumping MEP) * V_d / (4 pi)    (four-stroke: one cycle every 4 pi radians)
 *     rubbing MEP = 97 + 15 N + 5 N^2 kPa, N = rpm / 1000;  pumping MEP = max(p_amb - MAP, 0)
 * mdot_f comes from the car's fuel rate (PID 0x5E), else MAF / AFR, else the learned airflow constant (k * absolute load
 * * rpm / AFR). The engine size V_d = 12000 k / 1.184 L comes from the same learned k, so the estimate scales with the
 * engine of the car it runs in. In a decel fuel cut mdot_f = 0 and T_b = -T_f (engine braking).
 * A car that reports actual torque (PID 0x62) and reference torque (0x63) is used directly instead.
 * What learns per car: k (and so V_d), in rev_phys_row. eta_i is a prior; nothing on the board can learn it without a
 * torque label. Petrol only. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rev_learn.h"

#define REV_TQ_ETA_I     0.36f      /* gross indicated fuel conversion efficiency, prior */
#define REV_TQ_QLHV      44.0e6f    /* J/kg, petrol */
#define REV_TQ_RHO_F     0.745f     /* kg/L, as econ.c */
#define REV_TQ_AFR       14.7f
#define REV_TQ_PAMB      101.3f     /* kPa, when PID 0x33 is missing */
#define REV_TQ_RPM_RUN   400.0f     /* below: not running */
#define REV_TQ_RPM_MAX   8000.0f
#define REV_TQ_CUT_LOAD  24.0f      /* % calculated load: decel fuel-cut rule */
#define REV_TQ_RING      16
#define REV_TQ_SPAN_MS   5000
#define REV_TQ_VD_MIN    0.6f       /* L */
#define REV_TQ_VD_MAX    8.0f
#define REV_TQ_CONV_ROWS 30         /* fuel-cut rows of PID 0x62 before its convention is decided */

typedef enum { REV_TQ_NONE = 0, REV_TQ_PID, REV_TQ_RATE, REV_TQ_MAF, REV_TQ_K } rev_tq_src_t;
typedef enum { REV_CUT_NO = 0, REV_CUT_KNOWN, REV_CUT_RULE } rev_cut_t;

typedef struct {
    bool rpm_ok;   float rpm;
    bool speed_ok; float speed_kph, prev_speed_kph;
    bool rate_ok;  float rate_lph;      /* 0x5E */
    bool maf_ok;   float maf_gps;       /* 0x10 */
    bool abs_ok;   float abs_load;      /* 0x43 */
    bool load_ok;  float load;          /* 0x04, fuel-cut rule only */
    bool map_ok;   float map_kpa;       /* 0x0B */
    bool baro_ok;  float baro_kpa;      /* 0x33 */
    bool eqr_ok;   float eqr;           /* 0x44: 0 in a decel fuel cut */
    bool tq_ok;    float tq_pct;        /* 0x62 */
    bool ref_ok;   float ref_nm;        /* 0x63 */
    bool petrol;                        /* fuel type petrol or unknown; false: no physics torque */
} rev_tq_in_t;

typedef struct { float t_b, t_fuel, t_fric; rev_tq_src_t src; rev_cut_t cut; } rev_tq_row_t;

typedef struct {
    int64_t  t_ms[REV_TQ_RING];
    float    t_b[REV_TQ_RING], kph[REV_TQ_RING];
    uint8_t  src[REV_TQ_RING];
    int      head, n;
    int8_t   pid_conv;                  /* 0 unknown, 1 indicated (friction subtracted), 2 already brake torque */
    uint16_t pid_cut_n, pid_cut_neg;
    uint32_t rows[5];                   /* rows per source */
} rev_tq_t;

void      rev_tq_init(rev_tq_t *T);
void      rev_tq_new_drive(rev_tq_t *T);                  /* drop the 5 s window, keep the PID convention */
float     rev_tq_vd(const rev_phys_t *P);                 /* 12000 k / 1.184 L, clamped; from k0 until k has learned */
float     rev_tq_friction(float rpm, float map_kpa, bool map_ok, float baro_kpa, float vd_l);
float     rev_tq_fuel(float mdot_kgps, float rpm);        /* eta Q mdot / omega; 0 when not running */
rev_cut_t rev_tq_cut(const rev_tq_in_t *in);
/* One row: picks the source, applies the fuel cut, writes out. false when nothing usable. */
bool      rev_tq_row(rev_tq_t *T, const rev_tq_in_t *in, const rev_phys_t *P, rev_tq_row_t *out);
void      rev_tq_push(rev_tq_t *T, int64_t t_ms, const rev_tq_row_t *row, float kph);
/* Mean over (end - 5 s, end]: at least 2 rows spanning at least 3 s. show = false while the mean speed is under 2 km/h
   (at a standstill in Drive the model reads high: torque-converter drag is not in it). */
bool      rev_tq_mean(const rev_tq_t *T, int64_t end_ms, float *t_b, rev_tq_src_t *src, bool *show);
const char *rev_tq_src_name(rev_tq_src_t s);
