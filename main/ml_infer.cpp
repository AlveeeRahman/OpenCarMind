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

static const char *TAG = "ml";

/* Sliding inference: every ML_STRIDE_S seconds, on the last ML_WINDOW seconds (0 = once per 10 s pattern window). */
#define ML_STRIDE_S CONFIG_CARMIND_ML_STRIDE_S
/* A channel with no new sample keeps its last value for up to ML_HOLD_S seconds (mask stays 1), as in training
   (last value carried forward). Beyond that the bin is missing. */
#define ML_HOLD_S   CONFIG_CARMIND_ML_HOLD_S
#if CONFIG_CARMIND_ML_SYNTH_TEST
#define ML_ROW_MODEL "synth-" ML_MODEL_VERSION      /* bench rows must never pass for real driving data */
#else
#define ML_ROW_MODEL ML_MODEL_VERSION
#endif
extern const uint8_t ml_model_start[] asm("_binary_onboard_espdl_start");

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

static dl::Model     *s_model;
static ml_stats_t     s_stats = {false, 0, 0, 0, 0, 0, -1, 0.0f, {0.0f, 0.0f, 0.0f}, 0};
static float          s_last_p[ML_N_CLASSES];
static SemaphoreHandle_t s_stats_lock;
static sched_job_t    s_job = -1;

static void build_and_send(uint32_t window_seq, int64_t start_ms, int64_t end_ms);

extern "C" void ml_reset_history(void)
{
    memset(s_ring_n, 0, sizeof s_ring_n);
    s_t0_ms = -1;
    s_last_run_ms = 0;
}

extern "C" void ml_feed(const obd_values_t *v, int64_t now_ms)
{
    if (!s_model || s_paused) return;
    if (s_t0_ms < 0) s_t0_ms = now_ms;
    for (int c = 0; c < ML_CHANNELS; c++) {
        if (!ML_TRAINED[c] || !OBD_HAS(v, FIELDS[c])) continue;
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
            x[t][c] = m[t][c] ? (x[t][c] - ML_NORM_MEAN[c]) / ML_NORM_STD[c] : 0.0f;
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
    /* ESP-DL makes many small allocations (layers, tensor headers) and malloc puts anything under
       CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL in internal RAM, which TLS needs. Send them all to PSRAM while loading. */
    heap_caps_malloc_extmem_enable(0);
    /* nothrow: a failed top-level allocation means "model off", not an abort (ESP-DL's own allocations can still abort) */
    dl::Model *m = new (std::nothrow) dl::Model((const char *)ml_model_start, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    if (m && !m->get_input()) { delete m; m = nullptr; }
    return m;
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
        ok = s_model != nullptr;
        if (ok) ESP_LOGI(TAG, "model reloaded in %lld ms", (esp_timer_get_time() - t0) / 1000);
        else sys_event(EV_ERROR, "ML model failed to reload");
    }
    xSemaphoreGive(s_model_lock);
    xSemaphoreTake(s_stats_lock, portMAX_DELAY); s_stats.ready = ok; xSemaphoreGive(s_stats_lock);
    return ok;
}

static void ml_task(void *arg)
{
    (void)arg;
    char header[256];
    snprintf(header, sizeof header, "window_seq,start_ms,end_ms,model,manoeuvre,p_%s,p_%s,p_%s,p_%s,"
             "est_brake_pressure_bar,est_flywheel_torque_nm,est_fuel_rate_lph,latency_us,queue_ms\n",
             ML_CLASSES[0], ML_CLASSES[1], ML_CLASSES[2], ML_CLASSES[3]);
    for (;;) {
        size_t got = xMessageBufferReceive(s_mb, &s_run, sizeof s_run, pdMS_TO_TICKS(5000));
        if (got != sizeof s_run) {
            if (got == 0) sched_set_expected(s_job, false);   /* nothing for 5 s: no OBD link or model paused, not a stall */
            continue;
        }
        sched_set_expected(s_job, true);
        if (xSemaphoreTake(s_model_lock, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if (!s_model) { xSemaphoreGive(s_model_lock); continue; }     /* unloaded meanwhile: drop the window */
        sched_begin(s_job);
        dl::TensorBase *in = s_model->get_input();
        std::map<std::string, dl::TensorBase *> &outs = s_model->get_outputs();
        dl::TensorBase *man = outs["man"], *est = outs["est"];
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
        for (int k = 0; k < ML_N_ESTIMATES; k++) e[k] = value_at(est, k) * ML_EST_STD[k] + ML_EST_MEAN[k];
        xSemaphoreGive(s_model_lock);
        char line[256];
        snprintf(line, sizeof line, "%lu,%lld,%lld,%s,%s,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.1f,%lu,%lld\n",
                 (unsigned long)s_run.window_seq, (long long)s_run.start_ms, (long long)s_run.end_ms, ML_ROW_MODEL,
                 ML_CLASSES[best], p[0], p[1], p[2], p[3], e[0], e[1], e[2], (unsigned long)us,
                 (long long)((t0 - s_run.queued_us) / 1000));
        sdlog_write_ml_line(header, line);
#if CONFIG_CARMIND_ML_SYNTH_TEST
        ESP_LOGI(TAG, "%.*s", (int)strlen(line) - 1, line);
#endif
        xSemaphoreTake(s_stats_lock, portMAX_DELAY);
        s_stats.windows++; s_stats.last_us = us; if (us > s_stats.max_us) s_stats.max_us = us;
        s_stats.last_class = best; s_stats.last_prob = p[best];
        for (int k = 0; k < 3 && k < ML_N_ESTIMATES; k++) s_stats.last_est[k] = e[k];
        s_stats.last_at_ms = esp_timer_get_time() / 1000;
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
            if (!ML_TRAINED[c]) continue;
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

extern "C" void ml_init(void)
{
#if CONFIG_CARMIND_ML_ENABLE
    s_stats_lock = xSemaphoreCreateMutex();
    s_model_lock = xSemaphoreCreateMutex();
    s_mb = xMessageBufferCreateStatic(sizeof s_mb_store, s_mb_store, &s_mb_struct);
    int64_t t0 = esp_timer_get_time();
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_model = load_model();
    if (!s_model) { sys_event(EV_ERROR, "ML model failed to load"); return; }
    ESP_LOGI(TAG, "model %s (%s, %s) loaded in %lld ms, internal RAM %u B", ML_MODEL_VERSION, ML_QUANT, ML_TARGET_CHIP,
             (esp_timer_get_time() - t0) / 1000, (unsigned)(heap0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
#if CONFIG_CARMIND_ML_SELFTEST
    /* ESP-DL compares the model's output on its embedded test input with ESP-PPQ's simulation to 1 LSB. The S3 drifts
       about 2 % inside the trunk (measured), so a mismatch here is reported, not treated as a failure. */
    esp_err_t st = s_model->test();
    int64_t t1 = esp_timer_get_time();
    s_model->run();
    ESP_LOGI(TAG, "self-test %s (1-LSB check), one inference %lld us", st == ESP_OK ? "exact" : "within drift",
             esp_timer_get_time() - t1);
#endif
    s_job = sched_register("ml", (ML_STRIDE_S > 0 ? ML_STRIDE_S : 10) * 1000, (ML_STRIDE_S > 0 ? ML_STRIDE_S : 10) * 1000, true);
    sched_set_expected(s_job, false);              /* runs only while windows arrive */
    /* stack in PSRAM: the task only computes and never runs while the flash cache is disabled */
    if (xTaskCreatePinnedToCoreWithCaps(ml_task, "ml", 6144, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        sys_event(EV_ERROR, "ML task could not start (no memory)");
        return;
    }
    s_stats.ready = true;
    sys_event(EV_INFO, "ML model %s ready", ML_MODEL_VERSION);
#if CONFIG_CARMIND_ML_SYNTH_TEST
    xTaskCreatePinnedToCoreWithCaps(synth_task, "ml_synth", 4096, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
#endif
#endif
}
