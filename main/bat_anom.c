/* bat_anom: see bat_anom.h. Worker side: step() (never blocks). Board-task side: prepare(), board(). */
#include "bat_anom.h"
#if CONFIG_OCM_BAT_ANOM
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ai_snap.h"
#include "bat_anom_core.h"
#include "bat_anom_model.h"
#include "bat_est.h"
#include "events.h"
#include "status.h"

static const char *TAG = "banom";

#define BAN_PERIOD_MS   1000
#define BAN_DEADLINE_MS 1000
#define BAN_BUDGET_US   1000
#define BAN_SAMPLE_S    2.0f                    /* the board task reads the gauge every 2 s */
#define BAN_SAVE_US     (600LL * 1000000)
#define BAN_MAGIC       0x414E4F31u             /* "ANO1" */

/* the learnt part of the state (the window accumulators are not kept) */
typedef struct {
    uint32_t magic;
    float    tamb, ema_e, ema_r, ema_sag, sag_ok;
    float    med[AN_NF], mad[AN_NF], mu[AN_NF], var[AN_NF];
    float    nwin;
} ban_persist_t;

/* ---------- worker side ---------- */
EXT_RAM_BSS_ATTR static an_params_t s_p;
EXT_RAM_BSS_ATTR static an_state_t s_s;
static uint32_t s_seen;
static bool s_init, s_alarm;
static bat_anom_out_t s_out;
static ai_seq_t s_out_seq;
EXT_RAM_BSS_ATTR static ban_persist_t s_per;
static ai_seq_t s_per_seq;
static volatile uint32_t s_per_gen;
static struct { uint32_t head, tail; uint8_t kind[4]; char cls[4][32]; float score[4]; } s_evq;   /* worker -> board task */

/* ---------- board-task side ---------- */
EXT_RAM_BSS_ATTR static ban_persist_t s_saved;
static bool s_saved_ok;
static struct { char text[64]; int64_t t_ms; } s_last;
static ai_seq_t s_last_seq;

static void to_persist(const an_state_t *s, ban_persist_t *p)
{
    p->magic = BAN_MAGIC;
    p->tamb = s->tamb; p->ema_e = s->ema_e; p->ema_r = s->ema_r; p->ema_sag = s->ema_sag; p->sag_ok = s->sag_ok;
    memcpy(p->med, s->med, sizeof p->med); memcpy(p->mad, s->mad, sizeof p->mad);
    memcpy(p->mu, s->mu, sizeof p->mu); memcpy(p->var, s->var, sizeof p->var);
    p->nwin = s->nwin;
}

static bool persist_ok(const ban_persist_t *p)
{
    if (p->magic != BAN_MAGIC || !isfinite(p->nwin) || p->nwin < 0) return false;
    for (int j = 0; j < AN_NF; j++)
        if (!isfinite(p->med[j]) || !isfinite(p->mad[j]) || !isfinite(p->mu[j]) || !isfinite(p->var[j]) || p->var[j] < 0) return false;
    return isfinite(p->tamb) && isfinite(p->ema_e) && isfinite(p->ema_r) && isfinite(p->ema_sag);
}

static void evq_push(uint8_t kind, const char *cls, float score)
{
    uint32_t h = __atomic_load_n(&s_evq.head, __ATOMIC_RELAXED), t = __atomic_load_n(&s_evq.tail, __ATOMIC_ACQUIRE);
    if (h - t >= 4) return;
    s_evq.kind[h % 4] = kind;
    snprintf(s_evq.cls[h % 4], sizeof s_evq.cls[0], "%s", cls);
    s_evq.score[h % 4] = score;
    __atomic_store_n(&s_evq.head, h + 1, __ATOMIC_RELEASE);
}

static void publish(const an_out_t *o)
{
    bat_anom_out_t r = s_out;
    r.warm_need_h = s_p.warmup_win / 60.0f;
    r.warm_h = fminf(s_s.nwin, s_p.warmup_win) / 60.0f;
    r.thr_ae = s_p.thr_ae;
    if (o) { r.s_ae = o->s_ae; r.s_rz = o->s_rz; r.s_g = o->s_g; }
    r.windows = (uint32_t)s_s.nwin;
    r.state = s_alarm ? BAN_ALARM : s_s.nwin <= s_p.warmup_win ? BAN_WARMUP : BAN_NORMAL;
    if (!s_alarm) r.cls[0] = 0;
    AI_SEQ_PUT(&s_out_seq, &s_out, &r);
}

bool bat_anom_read(bat_anom_out_t *out) { return AI_SEQ_GET(&s_out_seq, out, &s_out) && out->state != BAN_OFF; }

static void step(int64_t release_us)
{
    (void)release_us;
    if (!s_init) {
        an_init(&s_p, &s_s, BAN_SAMPLE_S);
        if (s_saved_ok) {                              /* the learnt normal and the warm-up survive a restart */
            s_s.tamb = s_saved.tamb; s_s.ema_e = s_saved.ema_e; s_s.ema_r = s_saved.ema_r; s_s.ema_sag = s_saved.ema_sag;
            s_s.sag_ok = s_saved.sag_ok;
            memcpy(s_s.med, s_saved.med, sizeof s_s.med); memcpy(s_s.mad, s_saved.mad, sizeof s_s.mad);
            memcpy(s_s.mu, s_saved.mu, sizeof s_s.mu); memcpy(s_s.var, s_saved.var, sizeof s_s.var);
            s_s.nwin = s_saved.nwin;
        }
        s_init = true;
        publish(NULL);
    }
    bat_filt_t f;
    if (!bat_est_filter(&f) || f.n == s_seen) return;
    s_seen = f.n;
    an_out_t o;
    an_step(&s_p, &s_s, f.v, f.i, f.t_c, f.dt, f.valid, f.innov, f.ocv_z, f.v1, &o);
    if (!o.win_done) return;
    bool alarm = ((o.a_rz | (o.a_g << 1) | (o.a_ae << 2)) & BAT_ANOM_ALARM_MASK) != 0;   /* the autoencoder only */
    if (alarm && !s_alarm) {
        bat_anom_out_t r = s_out;
        snprintf(r.cls, sizeof r.cls, "%s", an_class_name(&o));
        r.alarms++;
        AI_SEQ_PUT(&s_out_seq, &s_out, &r);
        evq_push(1, r.cls, o.s_ae);
    } else if (!alarm && s_alarm) evq_push(2, "", o.s_ae);
    s_alarm = alarm;
    publish(&o);
    if (!s_alarm) {                                    /* the kept copy never holds an alarmed state */
        ban_persist_t p;
        to_persist(&s_s, &p);
        AI_SEQ_PUT(&s_per_seq, &s_per, &p);
        s_per_gen++;
    }
}

/* three windows on fixed data, 2 s samples (us) */
static uint32_t selftest(void)
{
    EXT_RAM_BSS_ATTR static an_state_t s;
    EXT_RAM_BSS_ATTR static an_params_t p;
    p = BAT_ANOM;
    an_init(&p, &s, BAN_SAMPLE_S);
    an_out_t o;
    uint32_t worst = 0;
    for (int k = 0; k < 90; k++) {
        int64_t t0 = esp_timer_get_time();
        an_step(&p, &s, 3.9f - 0.0001f * k, k & 1 ? -0.03f : -0.02f, 25.0f, 2.0f, 1, 0.001f, 3.9f, 0.002f, &o);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > worst) worst = us;
    }
    return worst;
}

static void prepare(void)
{
    s_p = BAT_ANOM;
    s_p.qnom_ah = CONFIG_OCM_BAT_NOMINAL_MAH / 1000.0f;   /* C-rates of this cell (the research cells were 3 Ah) */
    nvs_handle_t h;
    size_t n = sizeof s_saved;
    s_saved_ok = false;
    if (nvs_open("ocm_bat", NVS_READONLY, &h) == ESP_OK) {
        s_saved_ok = nvs_get_blob(h, "anom", &s_saved, &n) == ESP_OK && n == sizeof s_saved && persist_ok(&s_saved);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "anomaly detector: 60 s windows (%d samples of %.0f s), warm-up %.0f h, %s", (int)roundf(60.0f / BAN_SAMPLE_S),
             BAN_SAMPLE_S, s_p.warmup_win / 60.0, s_saved_ok ? "learnt normal restored" : "learning from the start");
    if (s_saved_ok) ESP_LOGI(TAG, "restored: %.0f windows (%.1f h of %.0f h warm-up)", s_saved.nwin,
                             fmin(s_saved.nwin, s_p.warmup_win) / 60.0, s_p.warmup_win / 60.0);
}

void bat_anom_last_event(char *text, int n, int32_t *age_s)
{
    typeof(s_last) cp, tmp;
    memset(&cp, 0, sizeof cp);
    ai_seq_read(&s_last_seq, &cp, &s_last, &tmp, sizeof cp);
    snprintf(text, (size_t)n, "%s", cp.text);
    *age_s = cp.text[0] ? (int32_t)((esp_timer_get_time() / 1000 - cp.t_ms) / 1000) : -1;
}

static void last_set(const char *t)
{
    ai_seq_write_begin(&s_last_seq);
    snprintf(s_last.text, sizeof s_last.text, "%s", t);
    s_last.t_ms = esp_timer_get_time() / 1000;
    ai_seq_write_end(&s_last_seq);
}

static void board(bool shutdown)
{
    static bool started;
    if (!started) {
        started = true;
        bat_anom_out_t o;
        if (bat_anom_read(&o) && o.state == BAN_WARMUP) last_set("Anomaly detector warming up (24 h)");
    }
    for (;;) {
        uint32_t t = __atomic_load_n(&s_evq.tail, __ATOMIC_RELAXED), h = __atomic_load_n(&s_evq.head, __ATOMIC_ACQUIRE);
        if (t == h) break;
        uint8_t k = s_evq.kind[t % 4];
        char cls[32];
        snprintf(cls, sizeof cls, "%s", s_evq.cls[t % 4]);
        float sc = s_evq.score[t % 4];
        __atomic_store_n(&s_evq.tail, t + 1, __ATOMIC_RELEASE);
        char b[64];
        if (k == 1) {
            sys_event(EV_WARN, "Battery anomaly: %s (score %.1f)", cls, (double)sc);
            snprintf(b, sizeof b, "Anomaly: %s", cls);
            STATUS_LOCK();                             /* the banner (a tap clears it); power is never touched */
            if (!g_status.alarm_text[0]) {
                snprintf(g_status.alarm_text, sizeof g_status.alarm_text, "Battery anomaly: %s", cls);
                g_status.alarm_critical = true;
                g_status.alarm_until_ms = INT64_MAX;
            }
            STATUS_UNLOCK();
        } else {
            sys_event(EV_INFO, "Battery anomaly over (score %.1f)", (double)sc);
            snprintf(b, sizeof b, "Anomaly over");
        }
        last_set(b);
    }
    static int64_t last_us = -1;
    static uint32_t written;
    int64_t now = esp_timer_get_time();
    if (last_us < 0) last_us = now;
    uint32_t g = s_per_gen;
    if (g == written || (!shutdown && now - last_us < BAN_SAVE_US)) return;
    ban_persist_t p;
    if (!AI_SEQ_GET(&s_per_seq, &p, &s_per) || !persist_ok(&p)) return;
    nvs_handle_t h;
    esp_err_t e = nvs_open("ocm_bat", NVS_READWRITE, &h);
    if (e == ESP_OK) { e = nvs_set_blob(h, "anom", &p, sizeof p); if (e == ESP_OK) e = nvs_commit(h); nvs_close(h); }
    last_us = now;
    written = g;
    ESP_LOGI(TAG, "learnt normal saved%s: %.0f windows: %s", shutdown ? " (shutdown)" : "", p.nwin, esp_err_to_name(e));
}

static const ai_svc_def_t SVC = {
    .name = "banom", .period_ms = BAN_PERIOD_MS, .deadline_ms = BAN_DEADLINE_MS, .budget_us = BAN_BUDGET_US,
    .step = step, .selftest = selftest, .prepare = prepare, .board = board,
};

const ai_svc_def_t *bat_anom_service(void) { return &SVC; }
#endif
