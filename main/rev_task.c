/* CarMind R.E.V on the board, see rev_task.h. */
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "rev_task.h"
#include "rev_state.h"
#include "ml_infer.h"
#include "sched_mon.h"
#include "sdlog.h"
#include "events.h"
#if CONFIG_CARMIND_REV_ENABLE
#include "ml_params_rev.h"
#define PRIOR_ID MLR_PRIOR_ID
#define REV_ON   1
#else
#define PRIOR_ID 0u
#define REV_ON   0
#endif
#if CONFIG_CARMIND_REV_PHYS
#define PHYS_ON  1
#else
#define PHYS_ON  0
#endif

#ifndef CONFIG_CARMIND_REV_SAVE_MIN
#define CONFIG_CARMIND_REV_SAVE_MIN 5
#endif
#ifndef CONFIG_CARMIND_ML_STRIDE_S
#define CONFIG_CARMIND_ML_STRIDE_S 1
#endif

static const char *TAG = "rev";
#define STATE_PATH   SDLOG_DIR "/rev_state.bin"
#define TANK_PATH    SDLOG_DIR "/rev_tank.bin"
#define TANK_MAGIC   0x4B4E4154u                      /* "TANK" */
#define SAVE_EVERY_MS ((int64_t)CONFIG_CARMIND_REV_SAVE_MIN * 60000)
#define QUEUE_LEN    4
#define CAL_MAGIC    0x31414352u                      /* "RCA1" */

typedef struct { float phi[REV_NF]; float y, label, prior, learned, phys; bool phys_ok; } rev_msg_t;
typedef struct { uint32_t magic; float c; uint32_t fill_rev; } cal_nvs_t;

#if CONFIG_CARMIND_REV_ENABLE
EXT_RAM_BSS_ATTR static rev_fuel_t s_fuel;           /* 75 KB: only the rev task writes it; the ml task reads the live weights */
#endif
EXT_RAM_BSS_ATTR static rev_phys_t s_phys;           /* written by the obd task, under s_lock */
EXT_RAM_BSS_ATTR static rev_cal_t  s_cal;            /* written by the obd and door tasks, under s_lock */
EXT_RAM_BSS_ATTR static rev_phys_t s_phys_snap;      /* copies taken under the lock for a save */
EXT_RAM_BSS_ATTR static rev_cal_t  s_cal_snap;
static rev_pick_t  s_pick;                           /* rev task; the obd task reads it without a lock (word-sized fields) */
static rev_label_t s_label;                          /* obd task only */
EXT_RAM_BSS_ATTR static uint8_t s_q_store[QUEUE_LEN * sizeof(rev_msg_t)];
static StaticQueue_t s_q_struct;
static QueueHandle_t s_q;
static SemaphoreHandle_t s_lock, s_saved_sem;
static TaskHandle_t s_task;
static sched_job_t s_job = -1;
static volatile bool s_learning, s_driving, s_save_req, s_reset_fuel_req;
static volatile float s_c = 1.0f;
static volatile uint32_t s_pairs, s_dropped, s_saves;
static volatile bool s_saved_ok;
static bool s_safe, s_ready;
static cal_nvs_t s_nv;                                      /* the calibration read from NVS at boot, merged when the card is read */
static bool s_have_nv;
static int64_t s_drive_start_ms, s_last_row_ms;
EXT_RAM_BSS_ATTR static rev_tank_t s_tank;           /* written by the obd and door tasks, under s_lock; both models */
EXT_RAM_BSS_ATTR static rev_tank_t s_tank_snap;
static rev_tq_t s_tq;                                /* obd task only */
static float s_prev_kph = -1;
static volatile float s_tq_nm = NAN;                 /* published by the obd task for the screen and the status view */
static volatile uint8_t s_tq_src;
static volatile bool s_tq_idle;
static float s_row_ms;

static bool lock(void) { return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE; }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* ---------------------------------------------------------------- calibration factor in NVS (survives a missing card and safe
   mode). Written only from the USB door task, which has an internal-RAM stack: NVS writes disable the flash cache. */
static bool cal_nvs_load(cal_nvs_t *out)
{
    nvs_handle_t h;
    if (nvs_open("rev", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof *out;
    bool ok = nvs_get_blob(h, "cal", out, &len) == ESP_OK && len == sizeof *out && out->magic == CAL_MAGIC &&
              isfinite(out->c) && out->c >= REV_CAL_MIN && out->c <= REV_CAL_MAX;
    nvs_close(h);
    return ok;
}

static void cal_nvs_store(float c, uint32_t fill_rev)
{
    cal_nvs_t v = {CAL_MAGIC, c, fill_rev};
    nvs_handle_t h;
    if (nvs_open("rev", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cal", &v, sizeof v);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------------------------------------------------------- the rev task */
static void save(void)
{
    if (!sdlog_ok()) { s_saved_ok = false; return; }
    if (!lock()) return;
    s_phys_snap = s_phys;
    s_cal_snap = s_cal;
    s_tank_snap = s_tank;
    unlock();
    int64_t t0 = esp_timer_get_time();
#if CONFIG_CARMIND_REV_ENABLE
    const rev_fuel_t *fuel = &s_fuel;
#else
    const rev_fuel_t *fuel = NULL;
#endif
    s_saved_ok = rev_state_save(STATE_PATH, fuel, &s_phys_snap, &s_pick, &s_cal_snap, PRIOR_ID);
    if (!rev_blob_save(TANK_PATH, TANK_MAGIC, &s_tank_snap, sizeof s_tank_snap)) ESP_LOGW(TAG, "tank state NOT saved");
    s_saves++;
    ESP_LOGI(TAG, "state %s in %lld ms", s_saved_ok ? "saved" : "NOT saved", (esp_timer_get_time() - t0) / 1000);
}

/* Read the learned state from the card once, in the rev task; under the lock because the obd task updates phys and cal. */
static void load_state(void)
{
    if (!sdlog_ok()) return;
#if CONFIG_CARMIND_REV_ENABLE
    rev_fuel_t *fuel = &s_fuel;
#else
    rev_fuel_t *fuel = NULL;
#endif
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rev_load_t how = rev_state_load(STATE_PATH, fuel, &s_phys, &s_pick, &s_cal, PRIOR_ID);
    bool tank = rev_blob_load(TANK_PATH, TANK_MAGIC, &s_tank, sizeof s_tank);
    if (tank && !rev_tank_valid(&s_tank)) { rev_tank_init(&s_tank); tank = false; }
    rev_tank_restart(&s_tank);
    if (s_have_nv && s_nv.fill_rev > s_cal.fill_rev) { s_cal.c = s_nv.c; s_cal.fill_rev = s_nv.fill_rev; }   /* NVS is newer than the card */
#if CONFIG_CARMIND_REV_CAL
    s_c = s_cal.c;
#endif
    xSemaphoreGive(s_lock);
    if (how == REV_LOAD_NONE) ESP_LOGI(TAG, "no learned state on the card: starting from the factory values");
    else if (how == REV_LOAD_OTHER_MODEL) sys_event(EV_INFO, "Fuel learning restarted: new R.E.V model");
    else ESP_LOGI(TAG, "learned state loaded%s", how == REV_LOAD_TMP ? " from the .tmp copy" : "");
    if (tank) ESP_LOGI(TAG, "tank learner state loaded");
    s_saved_ok = how != REV_LOAD_NONE;
}

static void rev_task(void *arg)
{
    (void)arg;
    static rev_msg_t m;
    load_state();
    bool commit_due = false, dirty = false;
    int64_t last_pair = 0, last_save = esp_timer_get_time() / 1000;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            sched_set_expected(s_job, true);
            last_pair = now;
            /* CPU reservation: wait for budget before each bounded step; meanwhile pairs queue up (a full queue drops) */
            while (!sched_cpu_available(s_job)) vTaskDelay(pdMS_TO_TICKS(50));
            sched_begin(s_job);
            rev_pick_score(&s_pick, m.label, true, m.prior, true, m.learned, m.phys_ok, m.phys);
#if CONFIG_CARMIND_REV_ENABLE
            if (rev_fuel_add(&s_fuel, m.phi, m.y)) commit_due = true;
#endif
            s_pairs++;
            dirty = true;
            sched_end(s_job);
        } else if (last_pair && now - last_pair > 5000) {
            sched_set_expected(s_job, false);                         /* no labels (no MAF, parked): not a stall */
            last_pair = 0;
        }
#if CONFIG_CARMIND_REV_ENABLE
        if (s_reset_fuel_req) {
            s_reset_fuel_req = false;
            rev_fuel_reset(&s_fuel);
            rev_pick_init(&s_pick);
            commit_due = false;
            s_save_req = true;
        }
        if (commit_due && sched_cpu_available(s_job)) {
            char msg[96];
            sched_begin(s_job);
            rev_commit_t r = rev_fuel_commit(&s_fuel, msg, sizeof msg);
            sched_end(s_job);
            commit_due = false;
            ESP_LOGI(TAG, "fuel head: %s (commits %lu, resets %lu)", msg, (unsigned long)s_fuel.commits, (unsigned long)s_fuel.resets);
            if (r == REV_RESET_PRIOR) sys_event(EV_INFO, "Fuel learning reset to factory");
        }
#else
        (void)commit_due;
#endif
        now = esp_timer_get_time() / 1000;
        if (s_save_req || (s_driving && dirty && now - last_save >= SAVE_EVERY_MS)) {
            bool asked = s_save_req;
            s_save_req = false;
            save();
            last_save = now;
            dirty = false;
            if (asked) xSemaphoreGive(s_saved_sem);
        }
    }
}

/* ---------------------------------------------------------------- setup */
void rev_init(void)
{
#if CONFIG_CARMIND_REV_ENABLE || CONFIG_CARMIND_REV_CAL
    s_lock = xSemaphoreCreateMutex();
    s_saved_sem = xSemaphoreCreateBinary();
    rev_phys_init(&s_phys);
    rev_cal_init(&s_cal);
    rev_pick_init(&s_pick);
    rev_label_reset(&s_label);
    rev_tank_init(&s_tank);
    rev_tq_init(&s_tq);
#if CONFIG_CARMIND_REV_ENABLE
    rev_fuel_init(&s_fuel, MLR_W0);
#endif
    cal_nvs_t nv;
    bool have_nv = cal_nvs_load(&nv);
    s_safe = sched_safe_mode();
    if (s_safe) {
        /* no learning, no state file (a bad file must not keep the board in a boot loop), c read-only from NVS */
        s_c = have_nv ? nv.c : 1.0f;
        ESP_LOGW(TAG, "safe mode: no learning, fuel calibration x%.3f from NVS", (double)s_c);
        return;
    }
    /* The SD state file is read by the rev task (6 KB stack), not here: FATFS reads and formatted logs overflow the 3.5 KB main
       task stack (seen on the board: "stack overflow in task main" right after the model self-test). NVS stays here, since the rev
       task's stack is in PSRAM and NVS runs with the flash cache disabled. Until the file is read, c comes from NVS. */
    s_nv = nv; s_have_nv = have_nv;
    if (have_nv && nv.fill_rev > s_cal.fill_rev) { s_cal.c = nv.c; s_cal.fill_rev = nv.fill_rev; }
    s_c = s_cal.c;
#if !CONFIG_CARMIND_REV_CAL
    s_c = 1.0f;
#endif
    s_q = xQueueCreateStatic(QUEUE_LEN, sizeof(rev_msg_t), s_q_store, &s_q_struct);
    /* Rate-monotonic order on core 1: obd (5) > ml (3) > rev (2). Pairs arrive at most once per model run, so the period is the
       model's; the deadline is the commit cadence, since a fold may wait for CPU budget. Not on the watchdog: the job also
       saves to the card, so sched_mon's I/O rule (restart after 3 x deadline) applies. */
    int stride = CONFIG_CARMIND_ML_STRIDE_S > 0 ? CONFIG_CARMIND_ML_STRIDE_S : 10;
    s_job = sched_register("rev", stride * 1000, 30000, false);
    sched_set_cpu_share(s_job, 20, 30);                              /* 2 % of a core on average, 30 ms bursts */
    sched_set_stall_ms(s_job, 60000);
    sched_set_expected(s_job, false);
    /* stack in PSRAM: computation and SD file I/O only, no NVS, so it never runs with the flash cache disabled */
    if (xTaskCreatePinnedToCoreWithCaps(rev_task, "rev", 6144, NULL, 2, &s_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        sys_event(EV_ERROR, "Fuel learning could not start (no memory)");
        return;
    }
    s_ready = true;
    s_learning = ml_rev_active();
#endif
}

void rev_model_changed(bool rev_active)
{
    s_learning = s_ready && rev_active;
    rev_label_reset(&s_label);
}

/* ---------------------------------------------------------------- obd task */
bool rev_wants_abs_load(void) { return PHYS_ON && s_learning; }

float rev_cal_factor(void) { return s_c; }

void rev_drive_started(void)
{
    rev_label_reset(&s_label);
    if (lock()) { rev_phys_new_drive(&s_phys); rev_tank_restart(&s_tank); unlock(); }
    rev_tq_new_drive(&s_tq);
    s_prev_kph = -1;
    s_tq_nm = NAN;
    s_drive_start_ms = esp_timer_get_time() / 1000;
    s_last_row_ms = 0;
    s_row_ms = 0;
    s_driving = true;
}

void rev_flush(uint32_t timeout_ms)
{
    if (!s_ready) return;
    xSemaphoreTake(s_saved_sem, 0);                                  /* clear a completion nobody waited for */
    s_save_req = true;
    xSemaphoreTake(s_saved_sem, pdMS_TO_TICKS(timeout_ms));
}

void rev_drive_ended(void)
{
    s_driving = false;
    if (!s_ready) return;
    s_save_req = true;                                               /* the rev task saves within a second */
}

void rev_estimates(const obd_values_t *row, rev_est_t *out)
{
    memset(out, 0, sizeof *out);
    out->prefer = FUEL_MODEL;
    if (!s_learning) return;
    bool phys_ready = false;
#if CONFIG_CARMIND_REV_PHYS
    if (lock()) {
        out->phys_ok = rev_phys_estimate(&s_phys, OBD_HAS(row, F_abs_load), row->f[F_abs_load], OBD_HAS(row, F_load), row->f[F_load],
                                         OBD_HAS(row, F_rpm), row->f[F_rpm], &out->phys_maf_gps, &phys_ready);
        unlock();
    }
#endif
    ml_stats_t ms;
    ml_get_stats(&ms);
    int64_t now = esp_timer_get_time() / 1000;
    out->learned_ok = ms.learned_ok && ms.last_at_ms >= s_drive_start_ms && now - ms.last_at_ms < 15000;
    out->learned_lph = ms.learned_lph;
    bool committed = false;
#if CONFIG_CARMIND_REV_ENABLE
    committed = s_fuel.commits > 0;
#endif
    rev_src_t s = rev_pick_choose(&s_pick, phys_ready, committed);
    out->prefer = s == REV_SRC_PHYS ? FUEL_PHYS : s == REV_SRC_LEARNED ? FUEL_LEARNED : FUEL_MODEL;
}

/* Torque from physics, both models: built from this row, averaged over 5 s, published for the screen and the CSV. */
static void torque_row(const obd_values_t *row, int64_t now_ms, float kph)
{
    int ft = OBD_HAS(row, F_fuel_type) ? (int)row->f[F_fuel_type] : 0;
    rev_tq_in_t in = {
        .rpm_ok = OBD_HAS(row, F_rpm), .rpm = row->f[F_rpm],
        .speed_ok = kph >= 0, .speed_kph = kph, .prev_speed_kph = s_prev_kph >= 0 ? s_prev_kph : kph,
        .rate_ok = OBD_HAS(row, F_fuel_rate), .rate_lph = row->f[F_fuel_rate],
        .maf_ok = OBD_HAS(row, F_maf), .maf_gps = row->f[F_maf],
        .abs_ok = OBD_HAS(row, F_abs_load), .abs_load = row->f[F_abs_load],
        .load_ok = OBD_HAS(row, F_load), .load = row->f[F_load],
        .map_ok = OBD_HAS(row, F_map), .map_kpa = row->f[F_map],
        .baro_ok = OBD_HAS(row, F_baro), .baro_kpa = row->f[F_baro],
        .eqr_ok = OBD_HAS(row, F_equiv), .eqr = row->f[F_equiv],
        .tq_ok = OBD_HAS(row, F_torque_actual), .tq_pct = row->f[F_torque_actual],
        .ref_ok = OBD_HAS(row, F_torque_ref), .ref_nm = row->f[F_torque_ref],
        .petrol = ft == 0 || ft == 1 || ft == 9 || ft == 16,      /* unknown, gasoline, bifuel gasoline, hybrid gasoline */
    };
    rev_tq_row_t r;
    bool ok = false;
    if (lock()) { ok = rev_tq_row(&s_tq, &in, &s_phys, &r); unlock(); }
    if (ok) rev_tq_push(&s_tq, now_ms, &r, kph);
    float tb; rev_tq_src_t src; bool show;
    if (rev_tq_mean(&s_tq, now_ms, &tb, &src, &show)) { s_tq_nm = tb; s_tq_src = (uint8_t)src; s_tq_idle = !show; }
    else s_tq_nm = NAN;
    s_prev_kph = kph;
}

void rev_row_outputs(float *torque_nm, uint8_t *torque_src, bool *torque_idle, float *tank_left_l, float *tank_range_km)
{
    *torque_nm = s_tq_nm; *torque_src = s_tq_src; *torque_idle = s_tq_idle;
    *tank_left_l = -1; *tank_range_km = -1;
    rev_tank_out_t o;
    if (!s_ready || !lock()) return;
    rev_tank_get(&s_tank, &o);
    unlock();
    if (o.ready && isfinite(o.left_raw_l)) { *tank_left_l = o.left_raw_l * s_c; *tank_range_km = o.range_km; }
}

void rev_row(const obd_values_t *row, int64_t now_ms, fuel_src_t src, float raw_lph, float dt_s, bool level_fresh)
{
    if (!s_ready) return;
    if (s_last_row_ms) { float gap = (float)(now_ms - s_last_row_ms); if (gap < 10000) s_row_ms = s_row_ms ? s_row_ms + (gap - s_row_ms) / 20 : gap; }
    s_last_row_ms = now_ms;
    bool rpm_ok = OBD_HAS(row, F_rpm);
    float kph = OBD_HAS(row, F_speed) ? row->f[F_speed] : -1;
#if CONFIG_CARMIND_REV_CAL
    if (lock()) {
        if (dt_s > 0) rev_cal_row(&s_cal, sdlog_session_id(), dt_s, raw_lph, kph, rpm_ok && row->f[F_rpm] > 0);
        unlock();
    }
#endif
    /* tank level and torque run with either model, like the fill-up calibration */
    if (lock()) {
        if (dt_s > 0) rev_tank_row(&s_tank, dt_s, raw_lph, kph, rpm_ok && row->f[F_rpm] > 0);
        if (level_fresh && OBD_HAS(row, F_fuel_level) && isfinite(row->f[F_fuel_level]))
            rev_tank_level(&s_tank, sdlog_session_id(), row->f[F_fuel_level]);
        unlock();
    }
    torque_row(row, now_ms, kph);
    if (!s_learning) return;
    ml_rev_in_t in = {0};
    if ((src == FUEL_PID || src == FUEL_MAF) && raw_lph >= 0) rev_label_push(&s_label, now_ms, raw_lph);
    in.label_ok = rev_label_mean(&s_label, now_ms, &in.label_lph);
#if CONFIG_CARMIND_REV_PHYS
    float maf_mean_est = 0;
    bool phys = false;
    if (lock()) {
        phys = rev_phys_row(&s_phys, now_ms, OBD_HAS(row, F_abs_load), row->f[F_abs_load], OBD_HAS(row, F_load), row->f[F_load],
                            rpm_ok, row->f[F_rpm], OBD_HAS(row, F_maf), row->f[F_maf], &maf_mean_est);
        unlock();
    }
    /* scored at the stoichiometric ratio: the label's own rows carry the commanded ratio, so fuel-cut seconds count against
       physics a little; that is the same error the estimate would make on screen with a stale ratio */
    if (phys) { in.phys_ok = true; in.phys_lph = econ_maf_lph(maf_mean_est, false, 0, OBD_HAS(row, F_fuel_type) ? (int)row->f[F_fuel_type] : 0); }
#endif
    ml_set_rev_inputs(&in);
}

/* ---------------------------------------------------------------- ml task */
bool rev_predict(const float phi[REV_NF], float *prior_std, float *learned_std)
{
#if CONFIG_CARMIND_REV_ENABLE
    if (!s_learning) return false;
    *prior_std = rev_fuel_predict_prior(&s_fuel, phi);
    *learned_std = rev_fuel_predict(&s_fuel, phi);
    return isfinite(*prior_std) && isfinite(*learned_std);
#else
    (void)phi; (void)prior_std; (void)learned_std;
    return false;
#endif
}

void rev_pair(const float phi[REV_NF], float y_std, float label_lph, float prior_lph, float learned_lph, bool phys_ok, float phys_lph)
{
    if (!s_q || !s_learning) return;
    static rev_msg_t m;                                              /* only the ml task sends */
    memcpy(m.phi, phi, sizeof m.phi);
    m.y = y_std; m.label = label_lph; m.prior = prior_lph; m.learned = learned_lph; m.phys_ok = phys_ok; m.phys = phys_lph;
    if (xQueueSend(s_q, &m, 0) != pdTRUE) s_dropped++;
}

/* ---------------------------------------------------------------- status */
void rev_get_status(rev_status_t *o)
{
    memset(o, 0, sizeof *o);
    o->enabled = REV_ON;
    o->learning = s_learning;
    o->safe = s_safe;
    o->saved = s_saved_ok;
    o->sd = sdlog_ok();
#if CONFIG_CARMIND_REV_ENABLE
    o->commits = s_fuel.commits; o->resets = s_fuel.resets; o->kept = s_fuel.kept; o->failed = s_fuel.failed; o->learned = s_fuel.learned;
    o->drift = rev_fuel_drift(&s_fuel);
    o->mse_live = s_fuel.mse_live; o->mse_prior = s_fuel.mse_prior;
#endif
    o->pairs = s_pairs; o->dropped = s_dropped; o->saves = s_saves;
    for (int i = 0; i < 3; i++) { o->err[i] = s_pick.e[i]; o->n_err[i] = s_pick.n[i]; }
    o->row_ms = s_row_ms;
    o->torque_nm = s_tq_nm; o->torque_src = s_tq_src; o->torque_idle = s_tq_idle; o->torque_ok = isfinite(o->torque_nm);
    if (lock()) {
        const rev_k_t *k = &s_phys.abs;
        if (!rev_k_ready(k)) for (int i = 0; i < REV_PHYS_BINS; i++) if (rev_k_ready(&s_phys.calc[i])) { k = &s_phys.calc[i]; o->k_calc = true; break; }
        o->k = k->k; o->k_ready = rev_k_ready(k); o->k_rows = k->n;
        o->prefer = rev_pick_choose(&s_pick, o->k_ready, o->commits > 0);
        o->cal = s_cal.c; o->fill_rev = s_cal.fill_rev; o->used = s_cal.used; o->skipped = s_cal.skipped; o->fills = s_cal.n_fills;
        o->life_l = s_cal.life_l; o->life_km = s_cal.life_km;
        rev_tank_get(&s_tank, &o->tank);
        o->tank_segs = s_tank.segs; o->tank_fills = s_tank.fills; o->tank_sug_n = s_tank.sug_n; o->tank_sug_seq = s_tank.sug_seq;
        memcpy(o->tank_sug, s_tank.sug, sizeof o->tank_sug);
        o->vd_l = rev_tq_vd(&s_phys);
        unlock();
    } else {
        o->cal = s_c;
    }
#if !CONFIG_CARMIND_REV_CAL
    o->cal = 1.0f;
#endif
}

bool rev_get_interval(int i, rev_interval_t *out)
{
    bool ok = false;
    if (lock()) {
        if (i >= 0 && i < s_cal.n_iv) { *out = s_cal.iv[i]; ok = true; }
        unlock();
    }
    return ok;
}

/* ---------------------------------------------------------------- usb door */
bool rev_set_fills(const rev_fill_t *fills, int n, uint32_t fill_rev, char *why, size_t why_n)
{
#if CONFIG_CARMIND_REV_CAL
    if (s_safe) { snprintf(why, why_n, "safe mode: the calibration is read-only"); return false; }
    if (!s_lock || !lock()) { snprintf(why, why_n, "busy, try again"); return false; }
    bool ok = rev_cal_set_fills(&s_cal, fills, n, fill_rev);
    float c = s_cal.c;
    int used = s_cal.used;
    unlock();
    if (!ok) { snprintf(why, why_n, "an entry is not plausible (litres 0-300, L/100 km 1-60, at most %d entries)", REV_CAL_FILLUPS); return false; }
    s_c = c;
    cal_nvs_store(c, fill_rev);
    if (used) sys_event(EV_INFO, "Fill-up applied: fuel x%.3f, %d tank%s", (double)c, used, used == 1 ? "" : "s");
    rev_flush(3000);
    return true;
#else
    (void)fills; (void)n; (void)fill_rev;
    snprintf(why, why_n, "fill-up calibration is not in this firmware");
    return false;
#endif
}

void rev_tank_suggestions_ack(uint32_t seq)
{
    if (s_safe || !s_lock) return;
    if (lock()) { rev_tank_ack(&s_tank, seq); unlock(); }
    rev_flush(3000);
}

bool rev_reset(const char *what)
{
    bool all = !strcmp(what, "all");
    if (s_safe || !s_lock) return false;
    if (!all && strcmp(what, "fuel") && strcmp(what, "phys") && strcmp(what, "cal") && strcmp(what, "tank")) return false;
    if (all || !strcmp(what, "fuel")) s_reset_fuel_req = true;
    if ((all || !strcmp(what, "phys") || !strcmp(what, "cal")) && lock()) {
        if (all || !strcmp(what, "phys")) rev_phys_reset_k(&s_phys);
        if (all || !strcmp(what, "cal")) { rev_cal_reset(&s_cal); s_c = 1.0f; }
        unlock();
    }
    if ((all || !strcmp(what, "tank")) && lock()) { rev_tank_reset(&s_tank); unlock(); }
    if (all || !strcmp(what, "cal")) cal_nvs_store(1.0f, 0);
    rev_flush(3000);
    return true;
}
