#include "mosaico_drive.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mosaico_clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "elm327.h"
#include "ml_infer.h"
#include "sdlog.h"
#include "status.h"
#include "events.h"
#include "mosaico_board.h"

static const char *TAG = "drive";
#define HARD_BRAKE_BAR 40.0f            /* model brake-pressure estimate counted as a hard brake (assumption, see report) */
#define DRIVES_CSV SDLOG_DRIVES_CSV                 /* the drive store: the NAND when it is mounted */

static SemaphoreHandle_t s_mx;
EXT_RAM_BSS_ATTR static md_summary_t s_pub, s_work;
EXT_RAM_BSS_ATTR static md_history_t s_hist, s_hist_work;
EXT_RAM_BSS_ATTR static app_status_t s_st;
static volatile bool s_end_req, s_start_req, s_hist_req, s_ending, s_auto_open;

/* ---- one series with averaging decimation ---- */
typedef struct { float *v; int n, step, acc_n; float acc; } series_t;
EXT_RAM_BSS_ATTR static float s_spd[MD_PTS], s_fuel[MD_PTS], s_cls_f[MD_PTS];
static series_t S_SPD = { .v = s_spd }, S_FUEL = { .v = s_fuel }, S_CLS = { .v = s_cls_f };

static void series_reset(series_t *s) { s->n = 0; s->step = 1; s->acc_n = 0; s->acc = 0; }
static void series_push(series_t *s, float x, bool last_wins)
{
    s->acc = last_wins ? x : s->acc + x;
    if (++s->acc_n < s->step) return;
    float v = last_wins ? s->acc : s->acc / s->acc_n;
    s->acc = 0; s->acc_n = 0;
    if (s->n == MD_PTS) {
        for (int i = 0; i < MD_PTS / 2; i++) s->v[i] = last_wins ? s->v[2 * i + 1] : (s->v[2 * i] + s->v[2 * i + 1]) / 2;
        s->n = MD_PTS / 2;
        s->step *= 2;
    }
    s->v[s->n++] = v;
}

/* ---- the drive being recorded ---- */
static struct {
    bool     on;
    uint32_t session, samples, idle, cls_n[4], ml_windows;
    int64_t  t0_ms, epoch0_ms;
    double   spd_sum;
    float    max_spd, km, fuel_l;
    int      last_cls, brake_events, hard_brakes;
    bool     hard_on;
} R;

static void rec_start(void)
{
    memset(&R, 0, sizeof R);
    R.on = true;
    R.session = sdlog_session_id();
    R.t0_ms = esp_timer_get_time() / 1000;
    R.last_cls = -1;
    if (mosaico_clock_known(&s_st)) R.epoch0_ms = (int64_t)time(NULL) * 1000;
    series_reset(&S_SPD); series_reset(&S_FUEL); series_reset(&S_CLS);
    ESP_LOGI(TAG, "recording drive s%06lu", (unsigned long)R.session);
}

static void rec_sample(void)
{
    const obd_values_t *o = &s_st.obd;
    bool spd_ok = OBD_HAS(o, F_speed), rpm_ok = OBD_HAS(o, F_rpm);
    if (!spd_ok && !rpm_ok) return;
    float v = spd_ok ? o->f[F_speed] : 0;
    ml_stats_t m;
    ml_get_stats(&m);
    R.samples++;
    R.spd_sum += v;
    if (v > R.max_spd) R.max_spd = v;
    if (v < 1.0f && rpm_ok && o->f[F_rpm] > 300) R.idle++;
    if (m.last_class >= 0 && m.last_class < 4) R.cls_n[m.last_class]++;
    if (m.windows != R.ml_windows && m.last_class >= 0) {      /* a new model window */
        R.ml_windows = m.windows;
        if (m.last_class == 1 && R.last_cls != 1) R.brake_events++;
        R.last_cls = m.last_class;
        bool hard = m.last_class == 1 && m.last_est[0] >= HARD_BRAKE_BAR;
        if (hard && !R.hard_on) R.hard_brakes++;
        R.hard_on = hard;
    }
    R.km = (float)o->trip_km;
    R.fuel_l = o->trip_fuel_l;
    series_push(&S_SPD, v, false);
    series_push(&S_FUEL, o->fuel_lph_calc >= 0 ? o->fuel_lph_calc : 0, false);
    series_push(&S_CLS, (float)m.last_class, true);
}

static void history_load(void)
{
    memset(&s_hist_work, 0, sizeof s_hist_work);
    FILE *f = fopen(DRIVES_CSV, "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            unsigned long sid; long long ep; long dur; float km, fl, l100, avg, mx, idle; int hb;
            if (sscanf(line, "%lu,%lld,%ld,%f,%f,%f,%f,%f,%f,%d", &sid, &ep, &dur, &km, &fl, &l100, &avg, &mx, &idle, &hb) != 10) continue;
            if (s_hist_work.n == MD_HIST) { memmove(&s_hist_work.d[0], &s_hist_work.d[1], (MD_HIST - 1) * sizeof s_hist_work.d[0]); s_hist_work.n--; }
            s_hist_work.d[s_hist_work.n++] = (typeof(s_hist_work.d[0])){ (uint32_t)sid, km, fl, l100 };
        }
        fclose(f);
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint32_t g = s_hist.gen + 1;
    s_hist = s_hist_work;
    s_hist.gen = g;
    xSemaphoreGive(s_mx);
}

static void rec_finish(void)
{
    R.on = false;
    md_summary_t *w = &s_work;
    memset(w, 0, sizeof *w);
    w->session = R.session;
    w->start_epoch_ms = R.epoch0_ms;
    w->dur_s = (int32_t)((esp_timer_get_time() / 1000 - R.t0_ms) / 1000);
    w->km = R.km; w->fuel_l = R.fuel_l;
    w->l100 = R.km > 0.2f && R.fuel_l > 0 ? 100.0f * R.fuel_l / R.km : -1;
    w->avg_kph = R.samples ? (float)(R.spd_sum / R.samples) : 0;
    w->max_kph = R.max_spd;
    w->idle_pct = R.samples ? 100.0f * R.idle / R.samples : 0;
    uint32_t cn = R.cls_n[0] + R.cls_n[1] + R.cls_n[2] + R.cls_n[3];
    for (int k = 0; k < 4; k++) w->cls_share[k] = cn ? (float)R.cls_n[k] / cn : 0;
    w->brake_events = R.brake_events; w->hard_brakes = R.hard_brakes;
    w->n = S_SPD.n;
    w->step_s = (float)S_SPD.step;
    memcpy(w->spd, s_spd, sizeof(float) * S_SPD.n);
    memcpy(w->fuel, s_fuel, sizeof(float) * S_FUEL.n);
    for (int i = 0; i < S_CLS.n && i < MD_PTS; i++) w->cls[i] = (int8_t)lroundf(s_cls_f[i]);
    w->ok = R.samples > 0;
    if (!w->ok) { ESP_LOGI(TAG, "drive s%06lu had no engine data: no summary", (unsigned long)R.session); return; }
    /* files: the summary next to the session, one history line */
    EXT_RAM_BSS_ATTR static char j[768];
    snprintf(j, sizeof j,
             "{\"schema\":1,\"session\":%lu,\"start_epoch_ms\":%lld,\"duration_s\":%ld,\"km\":%.2f,\"fuel_l\":%.3f,\"l_per_100km\":%.2f,"
             "\"avg_kph\":%.1f,\"max_kph\":%.1f,\"idle_pct\":%.1f,\"class_share\":{\"stopped\":%.3f,\"braking\":%.3f,"
             "\"accelerating\":%.3f,\"cruising\":%.3f},\"braking_events\":%d,\"hard_brakes\":%d,\"hard_brake_bar\":%.0f"
             "}\n",
             (unsigned long)w->session, (long long)w->start_epoch_ms, (long)w->dur_s, (double)w->km, (double)w->fuel_l, (double)w->l100,
             (double)w->avg_kph, (double)w->max_kph, (double)w->idle_pct, (double)w->cls_share[0], (double)w->cls_share[1],
             (double)w->cls_share[2], (double)w->cls_share[3], w->brake_events, w->hard_brakes, (double)HARD_BRAKE_BAR
             );
    char path[96];
    SDLOG_DRIVE_PATH(path, "/s%06lu.sum.json", (unsigned long)w->session);
    bool ok = sdlog_ok() && sdlog_save_text_atomic(path, j);
    struct stat st;
    bool fresh = stat(DRIVES_CSV, &st) != 0 || st.st_size == 0;
    FILE *f = sdlog_ok() ? fopen(DRIVES_CSV, "a") : NULL;
    if (f) {
        if (fresh) fputs("session,start_epoch_ms,duration_s,km,fuel_l,l_per_100km,avg_kph,max_kph,idle_pct,hard_brakes\n", f);
        fprintf(f, "%lu,%lld,%ld,%.2f,%.3f,%.2f,%.1f,%.1f,%.1f,%d\n", (unsigned long)w->session, (long long)w->start_epoch_ms,
                (long)w->dur_s, (double)w->km, (double)w->fuel_l, (double)w->l100, (double)w->avg_kph, (double)w->max_kph,
                (double)w->idle_pct, w->hard_brakes);
        fclose(f);
    }
    ESP_LOGI(TAG, "drive summary s%06lu: %ld s, %.1f km, %.2f L, %d hard brakes; %s %s, drives.csv %s",
             (unsigned long)w->session, (long)w->dur_s, (double)w->km, (double)w->fuel_l, w->hard_brakes,
             strrchr(path, '/') + 1, ok ? "written" : "NOT written", f ? "appended" : "NOT appended");
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint32_t g = s_pub.gen + 1;
    s_pub = *w;
    s_pub.gen = g;
    xSemaphoreGive(s_mx);
    history_load();
}

static void end_sequence(void)
{
    s_ending = true;
    sys_event(EV_INFO, "End drive pressed on the screen");
    elm_request_stop();                                 /* the OBD task closes the drive: model, R.E.V, session saved */
    bool stopped = elm_wait_stopped(8000);
    ESP_LOGI(TAG, "end drive: poll loop %s", stopped ? "stopped, drive closed" : "still busy after 8 s");
    if (R.on) { status_get(&s_st); rec_finish(); }
    s_auto_open = true;
    elm_resume();                                       /* the link may start the next drive (its own session) */
    s_ending = false;
}

static void drive_task(void *arg)
{
    (void)arg;
    history_load();
    TickType_t release = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&release, pdMS_TO_TICKS(1000));
        if (s_end_req) { s_end_req = false; end_sequence(); continue; }
        if (s_start_req) {
            s_start_req = false;
        }
        if (s_hist_req) { s_hist_req = false; history_load(); }
        bool active = elm_drive_active();
        status_get(&s_st);
        if (active && !R.on) rec_start();
        if (R.on && active) rec_sample();
        if (R.on && !active) rec_finish();              /* the car ended the drive (ignition off, link lost) */
    }
}

void mosaico_drive_start(void)
{
    s_mx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(drive_task, "drive_rec", 4096, NULL, 1, NULL, 0);
}

void mosaico_drive_end_request(void) { if (!s_ending) s_end_req = true; }
void mosaico_drive_start_request(void) { s_start_req = true; }
bool mosaico_drive_ending(void) { return s_ending || s_end_req; }
void mosaico_drive_history_request(void) { s_hist_req = true; }
bool mosaico_drive_take_auto_open(void) { bool a = s_auto_open; s_auto_open = false; return a; }

void mosaico_drive_get_summary(md_summary_t *out)
{
    if (!s_mx) { memset(out, 0, sizeof *out); return; }
    xSemaphoreTake(s_mx, portMAX_DELAY); *out = s_pub; xSemaphoreGive(s_mx);
}

void mosaico_drive_get_history(md_history_t *out)
{
    if (!s_mx) { memset(out, 0, sizeof *out); return; }
    xSemaphoreTake(s_mx, portMAX_DELAY); *out = s_hist; xSemaphoreGive(s_mx);
}

void mosaico_drive_live(int *brake_events, int *hard_brakes)
{
    *brake_events = R.on ? R.brake_events : 0;
    *hard_brakes = R.on ? R.hard_brakes : 0;
}
