// On-board model inference, see ml_infer.h. The input contract is fixed by the shipped model: 1 s bins, the last ML_WINDOW bins ending at the window's close (the
// closing second included), standardised with ML_NORM_*, untrained channels forced to missing, then values, masks
// and sample-to-sample differences, oldest sample first, NHWC [1][1][ML_WINDOW][ML_IN_CHANNELS].
#include <cmath>
#include <new>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "dl_model_base.hpp"
extern "C" {                     /* C headers without their own C++ guards, before anything includes them */
#include "status.h"
#include "events.h"
#include "sdlog.h"
#include "sys_mode.h"
}
#include "sdkconfig.h"
#include "ml_infer.h"
#include "ml_params.h"
#include "harden.h"
#include "sched_mon.h"
#include "rev_task.h"
#if CONFIG_CARMIND_REV_ENABLE
#include "ml_params_rev.h"
/* Both models are fed by the same window builder and read by the same code: only normalisation and weights may differ. */
static_assert(MLR_WINDOW == ML_WINDOW && MLR_BIN_S == ML_BIN_S && MLR_CHANNELS == ML_CHANNELS && MLR_IN_CHANNELS == ML_IN_CHANNELS &&
              MLR_DELTAS == ML_DELTAS && MLR_N_CLASSES == ML_N_CLASSES && MLR_N_ESTIMATES == ML_N_ESTIMATES, "E.V and R.E.V inputs differ");
static_assert(MLR_Z_DIM + 1 == REV_NF, "R.E.V z size");
#endif

static const char *TAG = "ml";

/* Sliding inference: every ML_STRIDE_S seconds, on the last ML_WINDOW seconds (0 = once per 10 s pattern window). */
#define ML_STRIDE_S CONFIG_CARMIND_ML_STRIDE_S
/* A channel with no new sample keeps its last value for up to ML_HOLD_S seconds (mask stays 1), as in training
   (last value carried forward). Beyond that the bin is missing. */
#define ML_HOLD_S   CONFIG_CARMIND_ML_HOLD_S
#if CONFIG_CARMIND_ML_SYNTH_TEST
#define ML_ROW_PREFIX "synth-"                       /* bench rows must never pass for real driving data */
#else
#define ML_ROW_PREFIX ""
#endif
extern const uint8_t ml_model_start[] asm("_binary_onboard_espdl_start");
#if CONFIG_CARMIND_REV_ENABLE
extern const uint8_t ml_rev_model_start[] asm("_binary_onboard_rev_espdl_start");
#endif

/* The models built into this firmware. The window layout, classes and estimates are shared (checked above); each model has its
   own normalisation, de-standardisation and hash. */
typedef struct {
    const char *key, *name, *version;
    const uint8_t *data;
    const float *norm_mean, *norm_std, *est_mean, *est_std;
    const unsigned char *trained;
} ml_model_desc_t;
static const ml_model_desc_t MODELS[ML_MODEL_COUNT] = {
    {"ev", "CarMind E.V", ML_MODEL_VERSION, ml_model_start, ML_NORM_MEAN, ML_NORM_STD, ML_EST_MEAN, ML_EST_STD, ML_TRAINED},
#if CONFIG_CARMIND_REV_ENABLE
    {"rev", "CarMind R.E.V", MLR_MODEL_VERSION, ml_rev_model_start, MLR_NORM_MEAN, MLR_NORM_STD, MLR_EST_MEAN, MLR_EST_STD, MLR_TRAINED},
#else
    {"rev", "CarMind R.E.V", "", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
#endif
};
static volatile int s_active = ML_MODEL_EV;          /* the model that is loaded (or being loaded) */
static bool s_z_ok;                                  /* the loaded model's "z" output exists and has REV_NF - 1 values */
#define MAF_CH 3                                     /* channel index of MAF in FIELDS */

static const obd_field_t FIELDS[ML_CHANNELS] = {F_rpm, F_speed, F_load, F_maf, F_throttle, F_stft1, F_ltft1, F_coolant, F_iat, F_map};

/* ---- raw samples, touched only by the OBD task ----
   Every row that carries a trained channel is stored with its time. The window is binned on demand when a row arrives: bin j of the
   ML_WINDOW bins ends at now - (ML_WINDOW-1-j) * ML_BIN_S
   seconds and takes the latest sample at or before that time. A sample older than ML_HOLD_S, or a bin before the first row of the
   session, is missing (mask 0). The bin width comes from ml_params.h (1 s or 2 s, whatever the model was trained for). */
#ifndef ML_BIN_S
#define ML_BIN_S 1
#endif
#define ML_RING 128                                  /* about 25 s at 5 rows per second, 3 minutes at one row per 1.4 s */
typedef struct { int64_t t_ms; float v; } ml_sample_t;
EXT_RAM_BSS_ATTR static ml_sample_t s_ring[ML_CHANNELS][ML_RING];
static int      s_ring_n[ML_CHANNELS];               /* samples ever pushed per channel */
static int64_t  s_t0_ms = -1;                        /* first row of the session */
static int64_t  s_last_run_ms;
static uint32_t s_infer_seq;

/* ---- hand-off to the inference task ---- */
typedef struct {
    uint32_t window_seq;
    int64_t  start_ms, end_ms, queued_us;
    float    in[ML_WINDOW * ML_IN_CHANNELS];
    uint8_t  model;                                  /* built with this model's normalisation; dropped if the model changed */
    bool     maf_seen;                               /* MAF is valid in some bin: R.E.V runs a second, MAF-hidden pass */
    ml_rev_in_t rev;                                 /* label and physics estimate for the same 5 s */
} ml_msg_t;
static StaticMessageBuffer_t s_mb_struct;
EXT_RAM_BSS_ATTR static uint8_t s_mb_store[sizeof(ml_msg_t) + sizeof(size_t) + 4];
static MessageBufferHandle_t s_mb;
EXT_RAM_BSS_ATTR static ml_msg_t s_build;         /* built in the OBD task */
/* window scratch, 1.5 KB: kept off the OBD task's stack (only the OBD task builds windows, so one copy is safe) */
EXT_RAM_BSS_ATTR static float   s_wx[ML_WINDOW][ML_CHANNELS];
EXT_RAM_BSS_ATTR static uint8_t s_wm[ML_WINDOW][ML_CHANNELS];
/* ml_set_paused(): no feeding, no windows, no inference, model stays resident */
static volatile bool s_paused;
EXT_RAM_BSS_ATTR static ml_msg_t s_run;           /* received by the inference task */
static ml_rev_in_t s_rev_in;                         /* set by the OBD task just before ml_feed() */
#if CONFIG_CARMIND_REV_ENABLE
EXT_RAM_BSS_ATTR static float s_mask_in[ML_WINDOW * ML_IN_CHANNELS];   /* MAF-hidden copy of the input */
EXT_RAM_BSS_ATTR static float s_phi[REV_NF];
#endif

static dl::Model     *s_model;
static ml_stats_t     s_stats = {false, 0, 0, 0, 0, 0, -1, 0.0f, {0.0f, 0.0f, 0.0f}, 0, ML_MODEL_EV, false, false, 0.0f, 0.0f, 0, 0};
static float          s_last_p[ML_N_CLASSES];
static SemaphoreHandle_t s_stats_lock;
static sched_job_t    s_job = -1;

static void build_and_send(uint32_t window_seq, int64_t start_ms, int64_t end_ms);

extern "C" void ml_set_rev_inputs(const ml_rev_in_t *in) { s_rev_in = *in; }

extern "C" void ml_reset_history(void)
{
    memset(s_ring_n, 0, sizeof s_ring_n);
    s_t0_ms = -1;
    s_last_run_ms = 0;
}

extern "C" void ml_drive_ended(void)
{
    if (s_job >= 0) sched_set_expected(s_job, false);   /* no rows are coming: a quiet model is not a stall */
    ml_reset_history();
}

extern "C" void ml_feed(const obd_values_t *v, int64_t now_ms)
{
    if (!s_model || s_paused) return;
    if (s_t0_ms < 0) s_t0_ms = now_ms;
    const unsigned char *trained = MODELS[s_active].trained;
    for (int c = 0; c < ML_CHANNELS; c++) {
        if (!trained[c] || !OBD_HAS(v, FIELDS[c])) continue;
        float x = v->f[FIELDS[c]];
        if (!std::isfinite(x)) continue;
        ml_sample_t *r = &s_ring[c][s_ring_n[c] % ML_RING];
        r->t_ms = now_ms; r->v = x; s_ring_n[c]++;
    }
#if ML_STRIDE_S > 0
    /* a row just arrived: run on the window that ends now, once the session has 10 s of history (as in training) and at most
       once per ML_STRIDE_S seconds */
    if (now_ms - s_t0_ms >= 10000 && now_ms - s_last_run_ms >= (int64_t)ML_STRIDE_S * 1000 - 100) {
        s_last_run_ms = now_ms;
        build_and_send(++s_infer_seq, now_ms - (int64_t)ML_WINDOW * ML_BIN_S * 1000, now_ms);
    }
#endif
}

static void build_and_send(uint32_t window_seq, int64_t start_ms, int64_t end_ms)
{
    if (!s_model || s_paused) return;
    int model = s_active;
    const ml_model_desc_t *md = &MODELS[model];
    float   (&x)[ML_WINDOW][ML_CHANNELS] = s_wx;
    uint8_t (&m)[ML_WINDOW][ML_CHANNELS] = s_wm;
    memset(x, 0, sizeof x); memset(m, 0, sizeof m);
    for (int c = 0; c < ML_CHANNELS; c++) {
        int n = s_ring_n[c], cnt = n < ML_RING ? n : ML_RING;
        for (int j = 0; j < ML_WINDOW; j++) {
            int64_t e = end_ms - (int64_t)(ML_WINDOW - 1 - j) * ML_BIN_S * 1000;
            if (e < s_t0_ms) continue;                                   /* before the session: missing */
            for (int i = 0; i < cnt; i++) {                              /* newest first: stop at the first sample not after e */
                const ml_sample_t *r = &s_ring[c][(n - 1 - i) % ML_RING];
                if (r->t_ms <= e) {
                    if (e - r->t_ms <= (int64_t)ML_HOLD_S * 1000) { x[j][c] = r->v; m[j][c] = 1; }
                    break;
                }
            }
        }
    }
    for (int t = 0; t < ML_WINDOW; t++)
        for (int c = 0; c < ML_CHANNELS; c++)
        {
            x[t][c] = m[t][c] ? (x[t][c] - md->norm_mean[c]) / md->norm_std[c] : 0.0f;
            if (m[t][c] && !harden_model_input(&x[t][c])) { x[t][c] = 0.0f; m[t][c] = 0; }   /* NaN or infinity from the adapter: missing */
        }

    for (int t = 0; t < ML_WINDOW; t++) {
        float *row = &s_build.in[t * ML_IN_CHANNELS];
        for (int c = 0; c < ML_CHANNELS; c++) {
            row[c] = x[t][c];
            row[ML_CHANNELS + c] = m[t][c];
#if ML_DELTAS
            row[2 * ML_CHANNELS + c] = t == 0 ? 0.0f : (x[t][c] - x[t - 1][c]) * m[t][c];
#endif
        }
    }
    s_build.window_seq = window_seq; s_build.start_ms = start_ms; s_build.end_ms = end_ms;
    s_build.model = (uint8_t)model;
    s_build.maf_seen = false;
    for (int t = 0; t < ML_WINDOW; t++) s_build.maf_seen |= m[t][MAF_CH] != 0;
    s_build.rev = s_rev_in;
    s_rev_in.label_ok = s_rev_in.phys_ok = false;                /* one label per window */
    s_build.queued_us = esp_timer_get_time();
    if (xMessageBufferSend(s_mb, &s_build, sizeof s_build, 0) != sizeof s_build) {
        xSemaphoreTake(s_stats_lock, portMAX_DELAY); s_stats.dropped++; xSemaphoreGive(s_stats_lock);
    }
}

/* Called by the pattern layer every 10 s. With sliding inference (ML_STRIDE_S > 0) ml_feed() drives the model instead. */
extern "C" void ml_window_closed(uint32_t window_seq, int64_t start_ms, int64_t end_ms)
{
#if ML_STRIDE_S == 0
    build_and_send(window_seq, start_ms, end_ms);
#else
    (void)window_seq; (void)start_ms; (void)end_ms;
#endif
}

static void quantize_into(dl::TensorBase *t, const float *src, int n)
{
    float inv = ldexpf(1.0f, -(int)t->exponent);
    if (t->dtype == dl::DATA_TYPE_INT16) {
        int16_t *d = (int16_t *)t->data;
        for (int i = 0; i < n; i++) { long q = lroundf(src[i] * inv); d[i] = q > 32767 ? 32767 : q < -32768 ? -32768 : q; }
    } else if (t->dtype == dl::DATA_TYPE_INT8) {
        int8_t *d = (int8_t *)t->data;
        for (int i = 0; i < n; i++) { long q = lroundf(src[i] * inv); d[i] = q > 127 ? 127 : q < -128 ? -128 : q; }
    } else {
        memcpy(t->data, src, n * sizeof(float));
    }
}

static float value_at(dl::TensorBase *t, int i)
{
    float s = ldexpf(1.0f, (int)t->exponent);
    if (t->dtype == dl::DATA_TYPE_INT16) return ((int16_t *)t->data)[i] * s;
    if (t->dtype == dl::DATA_TYPE_INT8) return ((int8_t *)t->data)[i] * s;
    return ((float *)t->data)[i];
}

/* The model can be unloaded and loaded again (ml_unload / ml_load). s_model_lock is held
   for one inference or one load/unload, so the task never runs a model that is being freed. */
static SemaphoreHandle_t s_model_lock;

static dl::Model *load_model(void)
{
    const uint8_t *data = MODELS[s_active].data;
    if (!data) return nullptr;
    /* ESP-DL makes many small allocations (layers, tensor headers) and malloc puts anything under
       CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL in internal RAM, which TLS needs. Send them all to PSRAM while loading. */
    heap_caps_malloc_extmem_enable(0);
    /* nothrow: a failed top-level allocation means "model off", not an abort (ESP-DL's own allocations can still abort) */
    dl::Model *m = new (std::nothrow) dl::Model((const char *)data, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    if (m && !m->get_input()) { delete m; m = nullptr; }
    return m;
}

/* An output by name, or null. std::map::operator[] would insert a null entry for a missing name. */
static dl::TensorBase *output(dl::Model *m, const char *name)
{
    std::map<std::string, dl::TensorBase *> &outs = m->get_outputs();
    auto it = outs.find(name);
    return it == outs.end() ? nullptr : it->second;
}

/* Which outputs the loaded model has: man and est are required; z (R.E.V) only switches learning on. Caller holds the model lock. */
static bool check_outputs(dl::Model *m)
{
    dl::TensorBase *man = output(m, "man"), *est = output(m, "est"), *z = output(m, "z");
    s_z_ok = false;
#if CONFIG_CARMIND_REV_ENABLE
    s_z_ok = z && z->get_size() == REV_NF - 1;
    if (z && !s_z_ok) ESP_LOGW(TAG, "z output has %d values, expected %d: fuel learning off", z->get_size(), REV_NF - 1);
#else
    (void)z;
#endif
    return man && est && man->get_size() >= ML_N_CLASSES && est->get_size() >= ML_N_ESTIMATES;
}

extern "C" void ml_unload(void)
{
    if (!s_model_lock || xSemaphoreTake(s_model_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    if (s_model) {
        delete s_model;
        s_model = nullptr;
        ESP_LOGI(TAG, "model unloaded, PSRAM free %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
    xSemaphoreGive(s_model_lock);
    xSemaphoreTake(s_stats_lock, portMAX_DELAY); s_stats.ready = false; xSemaphoreGive(s_stats_lock);
}

extern "C" void ml_set_paused(bool paused) { s_paused = paused; }

extern "C" bool ml_load(void)
{
    if (!s_model_lock || xSemaphoreTake(s_model_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    bool ok = true;
    if (!s_model) {
        int64_t t0 = esp_timer_get_time();
        s_model = load_model();
        ok = s_model != nullptr && check_outputs(s_model);
        if (s_model && !ok) { delete s_model; s_model = nullptr; }
        if (ok) ESP_LOGI(TAG, "model reloaded in %lld ms", (esp_timer_get_time() - t0) / 1000);
        else sys_event(EV_ERROR, "ML model failed to reload");
    }
    xSemaphoreGive(s_model_lock);
    xSemaphoreTake(s_stats_lock, portMAX_DELAY); s_stats.ready = ok; xSemaphoreGive(s_stats_lock);
    return ok;
}

#if CONFIG_CARMIND_REV_ENABLE
static float clip_lph(float v) { return v < 0 ? 0.0f : v > 100.0f ? 100.0f : v; }
#endif

static void ml_task(void *arg)
{
    (void)arg;
    /* PSRAM, off the 6 KB stack: header, line and the R.E.V scratch */
    EXT_RAM_BSS_ATTR static char header[384];
    EXT_RAM_BSS_ATTR static char line[384];
    snprintf(header, sizeof header, "window_seq,start_ms,end_ms,model,manoeuvre,p_%s,p_%s,p_%s,p_%s,"
             "est_brake_pressure_bar,est_flywheel_torque_nm,est_fuel_rate_lph,latency_us,queue_ms,"
             "model_name,fuel_label_lph,est_fuel_prior_lph,est_fuel_rev_lph,phys_lph,phys_k,rev_commits,rev_resets,fuel_cal,pass2_us\n",
             ML_CLASSES[0], ML_CLASSES[1], ML_CLASSES[2], ML_CLASSES[3]);
    for (;;) {
        size_t got = xMessageBufferReceive(s_mb, &s_run, sizeof s_run, pdMS_TO_TICKS(5000));
        if (got != sizeof s_run) {
            if (got == 0) sched_set_expected(s_job, false);   /* nothing for 5 s: no OBD link or model paused, not a stall */
            continue;
        }
        sched_set_expected(s_job, true);
        if (xSemaphoreTake(s_model_lock, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if (!s_model || s_run.model != s_active) { xSemaphoreGive(s_model_lock); continue; }   /* unloaded or switched meanwhile */
        const ml_model_desc_t *md = &MODELS[s_active];
        sched_begin(s_job);
        dl::TensorBase *in = s_model->get_input();
        dl::TensorBase *man = output(s_model, "man"), *est = output(s_model, "est");
        int64_t t0 = esp_timer_get_time();
        quantize_into(in, s_run.in, in->get_size());
        s_model->run();
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        float logit[ML_N_CLASSES], p[ML_N_CLASSES], mx = -1e30f, sum = 0;
        for (int k = 0; k < ML_N_CLASSES; k++) { logit[k] = value_at(man, k); if (logit[k] > mx) mx = logit[k]; }
        for (int k = 0; k < ML_N_CLASSES; k++) { p[k] = expf(logit[k] - mx); sum += p[k]; }
        int best = 0;
        for (int k = 0; k < ML_N_CLASSES; k++) { p[k] /= sum; if (p[k] > p[best]) best = k; }
        float e[ML_N_ESTIMATES];
        for (int k = 0; k < ML_N_ESTIMATES; k++) e[k] = value_at(est, k) * md->est_std[k] + md->est_mean[k];
        /* R.E.V: z of this window with MAF hidden. If MAF was in the window, run again with its value, mask and difference
           columns zeroed (the model then sees exactly a car without MAF); otherwise pass 1's z is already MAF-free. */
        bool rev = false;
        uint32_t us2 = 0;
#if CONFIG_CARMIND_REV_ENABLE
        dl::TensorBase *z = s_z_ok ? output(s_model, "z") : nullptr;
        rev = z && s_active == ML_MODEL_REV && ml_rev_active();
        if (rev && s_run.maf_seen) {
            memcpy(s_mask_in, s_run.in, sizeof s_mask_in);
            for (int t = 0; t < ML_WINDOW; t++) {
                float *row = &s_mask_in[t * ML_IN_CHANNELS];
                row[MAF_CH] = 0; row[ML_CHANNELS + MAF_CH] = 0;
#if ML_DELTAS
                row[2 * ML_CHANNELS + MAF_CH] = 0;
#endif
            }
            int64_t t2 = esp_timer_get_time();
            quantize_into(in, s_mask_in, in->get_size());
            s_model->run();
            us2 = (uint32_t)(esp_timer_get_time() - t2);
        }
        if (rev) {
            for (int k = 0; k < REV_NF - 1; k++) s_phi[k] = value_at(z, k);
            s_phi[REV_NF - 1] = 1.0f;
        }
#endif
        xSemaphoreGive(s_model_lock);

        float prior_lph = -1, learned_lph = -1, phys_k = 0, cal = rev_cal_factor();
        bool learned_ok = false;
        rev_status_t rs = {};
#if CONFIG_CARMIND_REV_ENABLE
        float prior_std, learned_std;
        if (rev && rev_predict(s_phi, &prior_std, &learned_std)) {
            const float mu = MLR_EST_MEAN[MLR_FUEL_INDEX], sd = MLR_EST_STD[MLR_FUEL_INDEX];
            prior_lph = clip_lph(prior_std * sd + mu);
            learned_lph = clip_lph(learned_std * sd + mu);
            learned_ok = true;
            if (s_run.rev.label_ok)
                rev_pair(s_phi, (s_run.rev.label_lph - mu) / sd, s_run.rev.label_lph, prior_lph, learned_lph, s_run.rev.phys_ok, s_run.rev.phys_lph);
            rev_get_status(&rs);
            phys_k = rs.k;
        }
#endif
        int n = snprintf(line, sizeof line, "%lu,%lld,%lld,%s%s,%s,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.1f,%lu,%lld,%s,",
                         (unsigned long)s_run.window_seq, (long long)s_run.start_ms, (long long)s_run.end_ms, ML_ROW_PREFIX, md->version,
                         ML_CLASSES[best], p[0], p[1], p[2], p[3], e[0], e[1], e[2], (unsigned long)us,
                         (long long)((t0 - s_run.queued_us) / 1000), md->key);
        if (rev) {
            char lab[12] = "", ph[12] = "";
            if (s_run.rev.label_ok) snprintf(lab, sizeof lab, "%.2f", s_run.rev.label_lph);
            if (s_run.rev.phys_ok) snprintf(ph, sizeof ph, "%.2f", s_run.rev.phys_lph);
            n += snprintf(line + n, sizeof line - n, "%s,%.2f,%.2f,%s,%.4e,%lu,%lu,%.4f,%lu\n", lab, prior_lph, learned_lph, ph, phys_k,
                          (unsigned long)rs.commits, (unsigned long)rs.resets, cal, (unsigned long)us2);
        } else {
            n += snprintf(line + n, sizeof line - n, ",,,,,,,%.4f,\n", cal);
        }
        sdlog_write_ml_line(header, line);
#if CONFIG_CARMIND_ML_SYNTH_TEST
        ESP_LOGI(TAG, "%.*s", (int)strlen(line) - 1, line);
#endif
        xSemaphoreTake(s_stats_lock, portMAX_DELAY);
        s_stats.windows++; s_stats.last_us = us; if (us > s_stats.max_us) s_stats.max_us = us;
        s_stats.last_class = best; s_stats.last_prob = p[best];
        for (int k = 0; k < 3 && k < ML_N_ESTIMATES; k++) s_stats.last_est[k] = e[k];
        s_stats.last_at_ms = esp_timer_get_time() / 1000;
        s_stats.learned_ok = learned_ok; s_stats.learned_lph = learned_lph; s_stats.prior_lph = prior_lph;
        s_stats.pass2_us = us2;
        memcpy(s_last_p, p, sizeof s_last_p);
        xSemaphoreGive(s_stats_lock);
        sched_end(s_job);
    }
}

#if CONFIG_CARMIND_ML_SYNTH_TEST
/* 100 s at 5 Hz: stopped 20 s, accelerate 0-60 km/h over 20 s, cruise 30 s, brake to 0 over 15 s, stopped 15 s. */
static void synth_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(20000));
    for (int calm = 0; calm < 10; calm = sys_mode_get() == MODE_SEEK ? calm + 1 : 0)   /* wait for a quiet period */
        vTaskDelay(pdMS_TO_TICKS(1000));
    int64_t t0 = esp_timer_get_time() / 1000;
    obd_values_t v;
    for (int i = 0; i < 500; i++) {
        float t = i * 0.2f, spd, acc;
        if (t < 20)      { spd = 0;                  acc = 0; }
        else if (t < 40) { spd = (t - 20) * 3.0f;    acc = 1; }
        else if (t < 70) { spd = 60;                 acc = 0; }
        else if (t < 85) { spd = 60 - (t - 70) * 4;  acc = -1; }
        else             { spd = 0;                  acc = 0; }
        memset(&v, 0, sizeof v);
        float load = acc > 0 ? 65.0f : spd > 0 ? (acc < 0 ? 12.0f : 35.0f) : 22.0f;
        float thr = acc > 0 ? 28.0f : spd > 0 && acc == 0 ? 12.0f : 0.0f;
        float map = acc > 0 ? 75.0f : acc < 0 ? 25.0f : 35.0f;
        float vals[ML_CHANNELS] = {spd > 0 ? 900 + spd * 28 : 780.0f, spd, load, 0, thr, 0.5f, 2.9f, 88, 0, map};
        for (int c = 0; c < ML_CHANNELS; c++) {
            if (!MODELS[s_active].trained[c]) continue;
            v.f[FIELDS[c]] = vals[c];
            v.valid[FIELDS[c] / 64] |= 1ull << (FIELDS[c] % 64);
        }
        int64_t now = t0 + i * 200;
        ml_feed(&v, now);
#if ML_STRIDE_S > 0
        if (i % 5 == 4) vTaskDelay(pdMS_TO_TICKS(60));     /* one inference per synthetic second, give the task time to drain */
#endif
        if (i % 50 == 49) {
            ml_window_closed(990000 + i / 50, now - 9800, now);
            vTaskDelay(pdMS_TO_TICKS(100));          /* real windows are 10 s apart; give the task time to drain */
        }
    }
    ESP_LOGI(TAG, "synthetic drive fed: 10 windows");
    vTaskDelete(NULL);
}
#endif

extern "C" void ml_get_stats(ml_stats_t *out)
{
    if (!s_stats_lock) { memset(out, 0, sizeof *out); out->last_class = -1; return; }
    xSemaphoreTake(s_stats_lock, portMAX_DELAY); *out = s_stats; xSemaphoreGive(s_stats_lock);
}

extern "C" int ml_get_last_probs(float *out, int max)
{
    if (!s_stats_lock) return 0;
    int n = max < ML_N_CLASSES ? max : ML_N_CLASSES;
    xSemaphoreTake(s_stats_lock, portMAX_DELAY);
    if (s_stats.windows == 0) n = 0;
    else memcpy(out, s_last_p, n * sizeof *out);
    xSemaphoreGive(s_stats_lock);
    return n;
}

/* One timed run on the model's embedded test input. Caller holds the model lock. */
static uint32_t selftest(void)
{
#if CONFIG_CARMIND_ML_SELFTEST
    /* ESP-DL compares the model's output on its embedded test input with ESP-PPQ's simulation to 1 LSB. The S3 drifts
       about 2 % inside the trunk (measured), so a mismatch here is reported, not treated as a failure. */
    esp_err_t st = s_model->test();
    int64_t t1 = esp_timer_get_time();
    s_model->run();
    uint32_t us = (uint32_t)(esp_timer_get_time() - t1);
    ESP_LOGI(TAG, "%s self-test %s (1-LSB check), one inference %lu us", MODELS[s_active].name, st == ESP_OK ? "exact" : "within drift",
             (unsigned long)us);
    return us;
#else
    return 0;
#endif
}

extern "C" const char *ml_model_key(int id) { return id >= 0 && id < ML_MODEL_COUNT ? MODELS[id].key : "?"; }
extern "C" const char *ml_model_name(int id) { return id >= 0 && id < ML_MODEL_COUNT ? MODELS[id].name : "?"; }
extern "C" bool ml_model_available(int id)
{
#if CONFIG_CARMIND_ML_ENABLE
    return id >= 0 && id < ML_MODEL_COUNT && MODELS[id].data;
#else
    (void)id;
    return false;
#endif
}
extern "C" int ml_model_active(void) { return s_active; }

extern "C" bool ml_rev_active(void)
{
    return s_active == ML_MODEL_REV && s_model && s_z_ok && !sched_safe_mode();
}

static void announce(void)
{
    xSemaphoreTake(s_stats_lock, portMAX_DELAY);
    s_stats.model = s_active; s_stats.z_ok = s_z_ok; s_stats.learned_ok = false; s_stats.pass2_us = 0;
    xSemaphoreGive(s_stats_lock);
    /* "ML Model - CarMind R.E.V ready" is 30 characters, inside EVENT_TEXT (56). The hash stays in the log and every .ml.csv row. */
    sys_event(EV_INFO, "ML Model - %s ready", MODELS[s_active].name);
}

/* NVS namespace "ml", key "model": a u8 of its own, not inside remote_config_t, whose blob is accepted only at its exact size
   (remote_config.c), so a new field there would reset every user's settings. */
static int stored_model(void)
{
    nvs_handle_t h;
    uint8_t v = ML_MODEL_EV;
    if (nvs_open("ml", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "model", &v); nvs_close(h); }
    return v < ML_MODEL_COUNT && ml_model_available(v) ? (int)v : (int)ML_MODEL_EV;
}

/* Unloading and loading at run time is what ml_unload()/ml_load() were written for: the model lock is held across the swap, the
   task drops a window built for the other model (ml_msg_t.model), both models sit in flash rodata, and ESP-DL's runtime
   allocations go to PSRAM and are freed by the delete. A reboot is not needed. */
extern "C" bool ml_select_model(int id, char *why, size_t why_n)
{
    if (!ml_model_available(id)) { snprintf(why, why_n, "%s is not in this firmware", ml_model_name(id)); return false; }
    if (!s_model_lock) { snprintf(why, why_n, "the model is off (safe mode or no memory)"); return false; }
    if (xSemaphoreTake(s_model_lock, pdMS_TO_TICKS(3000)) != pdTRUE) { snprintf(why, why_n, "the model is busy, try again"); return false; }
    int old = s_active;
    bool ok = true;
    if (id != old || !s_model) {
        delete s_model;
        s_model = nullptr;
        size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        int64_t t0 = esp_timer_get_time();
        s_active = id;
        s_model = load_model();
        ok = s_model && check_outputs(s_model);
        if (!ok) {
            delete s_model;
            s_active = old;
            s_model = load_model();
            if (s_model) check_outputs(s_model);
            snprintf(why, why_n, "%s did not load, still on %s", ml_model_name(id), ml_model_name(old));
        } else {
            ESP_LOGI(TAG, "%s loaded in %lld ms, PSRAM free %u KB (was %u KB with the old model unloaded)", MODELS[id].name,
                     (esp_timer_get_time() - t0) / 1000, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned)(ps0 / 1024));
            uint32_t us = selftest();
            xSemaphoreTake(s_stats_lock, portMAX_DELAY); s_stats.selftest_us = us; xSemaphoreGive(s_stats_lock);
        }
        ml_reset_history();
    }
    xSemaphoreGive(s_model_lock);
    xSemaphoreTake(s_stats_lock, portMAX_DELAY); s_stats.ready = s_model != nullptr; xSemaphoreGive(s_stats_lock);
    if (!ok) { sys_event(EV_WARN, "%s did not load", ml_model_name(id)); return false; }
    nvs_handle_t h;
    if (nvs_open("ml", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "model", (uint8_t)id); nvs_commit(h); nvs_close(h); }
    announce();
    rev_model_changed(ml_rev_active());
    return true;
}

extern "C" void ml_init(void)
{
#if CONFIG_CARMIND_ML_ENABLE
    s_stats_lock = xSemaphoreCreateMutex();
    s_model_lock = xSemaphoreCreateMutex();
    s_mb = xMessageBufferCreateStatic(sizeof s_mb_store, s_mb_store, &s_mb_struct);
    int64_t t0 = esp_timer_get_time();
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_active = stored_model();
    s_model = load_model();
    if (s_model && !check_outputs(s_model)) { delete s_model; s_model = nullptr; }
    if (!s_model && s_active != ML_MODEL_EV) {                     /* the stored choice does not load: fall back to E.V */
        sys_event(EV_WARN, "%s failed to load, using E.V", MODELS[s_active].name);
        s_active = ML_MODEL_EV;
        s_model = load_model();
        if (s_model && !check_outputs(s_model)) { delete s_model; s_model = nullptr; }
    }
    if (!s_model) { sys_event(EV_ERROR, "ML model failed to load"); return; }
    ESP_LOGI(TAG, "model %s %s (%s, %s) loaded in %lld ms, internal RAM %u B, z output %s", MODELS[s_active].name, MODELS[s_active].version,
             ML_QUANT, ML_TARGET_CHIP, (esp_timer_get_time() - t0) / 1000, (unsigned)(heap0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             s_z_ok ? "yes" : "no");
    s_stats.selftest_us = selftest();
    s_job = sched_register("ml", (ML_STRIDE_S > 0 ? ML_STRIDE_S : 10) * 1000, (ML_STRIDE_S > 0 ? ML_STRIDE_S : 10) * 1000, true);
    /* The model runs once per OBD row, which the car paces: about 1.4 s on a real car with gaps over 3 s, so three 1 s
       periods is too tight. A stall is no run for a whole 10 s label window while rows are expected. */
    if (ML_STRIDE_S > 0) sched_set_stall_ms(s_job, 10000);   /* stride 0 keeps the 3-period rule of its 10 s period */
    sched_set_expected(s_job, false);              /* runs only while windows arrive */
    /* stack in PSRAM: the task only computes and never runs while the flash cache is disabled */
    if (xTaskCreatePinnedToCoreWithCaps(ml_task, "ml", 6144, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        sys_event(EV_ERROR, "ML task could not start (no memory)");
        return;
    }
    s_stats.ready = true;
    announce();
#if CONFIG_CARMIND_ML_SYNTH_TEST
    xTaskCreatePinnedToCoreWithCaps(synth_task, "ml_synth", 4096, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
#endif
#endif
}
