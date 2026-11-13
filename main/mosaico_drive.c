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
#include "stop_core.h"
#include "rev_task.h"
#include "obd_ble.h"

static const char *TAG = "drive";
#define HARD_BRAKE_BAR 40.0f            /* model brake-pressure estimate counted as a hard brake (assumption, see report) */
#define DRIVES_CSV SDLOG_DRIVES_CSV                 /* the drive store: the NAND when it is mounted */

static SemaphoreHandle_t s_mx;
EXT_RAM_BSS_ATTR static md_summary_t s_pub, s_work;
EXT_RAM_BSS_ATTR static md_history_t s_hist, s_hist_work;
EXT_RAM_BSS_ATTR static app_status_t s_st;
static volatile bool s_end_req, s_start_req, s_hist_req, s_ending, s_auto_open;
static volatile bool s_retry_req, s_turnoff_req;

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
    int      nev;
    int64_t  ev_seen;
} R;
EXT_RAM_BSS_ATTR static sys_event_t s_rev[UEV_DRIVE_EVENTS];   /* the drive's own events (uev_collect), oldest first */

/* the ring's new entries into the drive's list: the 10-entry ring alone loses a long drive's early events */
static void rec_events(void)
{
    R.nev = uev_collect(s_rev, R.nev, UEV_DRIVE_EVENTS, s_st.events, s_st.event_head, s_st.event_count, R.t0_ms, &R.ev_seen,
                        s_st.wifi_ssid);
}

static void rec_start(void)
{
    memset(&R, 0, sizeof R);
    R.on = true;
    R.session = sdlog_session_id();
    R.t0_ms = esp_timer_get_time() / 1000;
    R.last_cls = -1;
    R.ev_seen = -1;
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
    rec_events();
    R.on = false;
    md_summary_t *w = &s_work;
    memset(w, 0, sizeof *w);
    w->session = R.session;
    w->start_epoch_ms = R.epoch0_ms;
    w->start_up_ms = R.t0_ms;
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
    w->nev = R.nev;
    memcpy(w->ev, s_rev, sizeof s_rev[0] * (size_t)R.nev);
    w->ok = R.samples > 0;
    if (!w->ok) { ESP_LOGI(TAG, "drive s%06lu had no engine data: no summary", (unsigned long)R.session); return; }
    /* files: the summary next to the session, one history line */
    EXT_RAM_BSS_ATTR static char j[768 + 24 * 100], evj[24 * 100];
    if (!uev_events_json(w->ev, w->nev, evj, sizeof evj)) evj[0] = 0;   /* (fits: 24 x at most ~95 B) */
    snprintf(j, sizeof j,
             "{\"schema\":1,\"session\":%lu,\"start_epoch_ms\":%lld,\"duration_s\":%ld,\"km\":%.2f,\"fuel_l\":%.3f,\"l_per_100km\":%.2f,"
             "\"avg_kph\":%.1f,\"max_kph\":%.1f,\"idle_pct\":%.1f,\"class_share\":{\"stopped\":%.3f,\"braking\":%.3f,"
             "\"accelerating\":%.3f,\"cruising\":%.3f},\"braking_events\":%d,\"hard_brakes\":%d,\"hard_brake_bar\":%.0f,"
             "\"start_up_ms\":%lld%s}\n",
             (unsigned long)w->session, (long long)w->start_epoch_ms, (long)w->dur_s, (double)w->km, (double)w->fuel_l, (double)w->l100,
             (double)w->avg_kph, (double)w->max_kph, (double)w->idle_pct, (double)w->cls_share[0], (double)w->cls_share[1],
             (double)w->cls_share[2], (double)w->cls_share[3], w->brake_events, w->hard_brakes, (double)HARD_BRAKE_BAR,
             (long long)w->start_up_ms, evj);
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
    ESP_LOGI(TAG, "drive summary s%06lu: %ld s, %.1f km, %.2f L, %d hard brakes, %d events; %s %s, drives.csv %s",
             (unsigned long)w->session, (long)w->dur_s, (double)w->km, (double)w->fuel_l, w->hard_brakes, w->nev,
             strrchr(path, '/') + 1, ok ? "written" : "NOT written", f ? "appended" : "NOT appended");
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint32_t g = s_pub.gen + 1;
    s_pub = *w;
    s_pub.gen = g;
    xSemaphoreGive(s_mx);
    history_load();
}

/* After a restart: the last drive in drives.csv back from its files, so the Summary shows it with its events. The events
   come from its .sys.txt when the firmware writes one (rows inside the drive's window, the test firmware), else from
   the "events" the .sum.json carries. No curves (they are not in the files). */
static bool drive_file(const char *name, char *path, size_t n)
{
#if CONFIG_OCM_NAND_LOGS
    return sdlog_drive_file(name, path, n);
#else
    snprintf(path, n, "%s/%s", SDLOG_DRIVE_DIR, name);
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static void past_load(uint32_t session)
{
    EXT_RAM_BSS_ATTR static char j[768 + 24 * 100 + 64];
    char name[24], path[96];
    snprintf(name, sizeof name, "s%06lu.sum.json", (unsigned long)session);
    FILE *f = drive_file(name, path, sizeof path) ? fopen(path, "r") : NULL;
    if (!f) return;
    size_t r = fread(j, 1, sizeof j - 1, f);
    fclose(f);
    j[r] = 0;
    md_summary_t *w = &s_work;
    memset(w, 0, sizeof *w);
    w->session = (uint32_t)uev_json_num(j, "session", 0);
    if (w->session != session) return;
    w->past = true;
    w->start_epoch_ms = (int64_t)uev_json_num(j, "start_epoch_ms", 0);
    w->start_up_ms = (int64_t)uev_json_num(j, "start_up_ms", 0);
    w->dur_s = (int32_t)uev_json_num(j, "duration_s", 0);
    w->km = (float)uev_json_num(j, "km", 0); w->fuel_l = (float)uev_json_num(j, "fuel_l", 0);
    w->l100 = (float)uev_json_num(j, "l_per_100km", -1);
    w->avg_kph = (float)uev_json_num(j, "avg_kph", 0); w->max_kph = (float)uev_json_num(j, "max_kph", 0);
    w->idle_pct = (float)uev_json_num(j, "idle_pct", 0);
    static const char *const CS[4] = {"stopped", "braking", "accelerating", "cruising"};
    for (int k = 0; k < 4; k++) w->cls_share[k] = (float)uev_json_num(j, CS[k], 0);
    w->brake_events = (int)uev_json_num(j, "braking_events", 0);
    w->hard_brakes = (int)uev_json_num(j, "hard_brakes", 0);
    snprintf(name, sizeof name, "s%06lu.sys.txt", (unsigned long)session);
    f = w->start_up_ms > 0 && drive_file(name, path, sizeof path) ? fopen(path, "r") : NULL;
    const char *from = "sum.json";
    if (f) {
        int64_t a = w->start_up_ms, z = w->start_up_ms + (int64_t)w->dur_s * 1000 + UEV_DRIVE_TAIL_MS;
        char line[200];
        sys_event_t e;
        while (fgets(line, sizeof line, f)) {
            if (!uev_sys_line(line, &e) || e.uptime_ms < a || e.uptime_ms > z) continue;
            if (w->nev == UEV_DRIVE_EVENTS) { memmove(&w->ev[0], &w->ev[1], (UEV_DRIVE_EVENTS - 1) * sizeof w->ev[0]); w->nev--; }
            w->ev[w->nev++] = e;
        }
        fclose(f);
        from = "sys.txt";
    } else w->nev = uev_events_parse(j, w->ev, UEV_DRIVE_EVENTS);
    w->ok = true;
    ESP_LOGI(TAG, "last drive s%06lu read back: %ld s, %.1f km, %d events (from its %s)", (unsigned long)session, (long)w->dur_s,
             (double)w->km, w->nev, from);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint32_t g = s_pub.gen + 1;
    if (!s_pub.ok) { s_pub = *w; s_pub.gen = g; }       /* a drive of this boot wins */
    xSemaphoreGive(s_mx);
}

/* ---- stop logging (OpenCarMind EdgeOS 2.1.1 field fix): every stop path ends READY (verified safe to unplug) or FAILED
   inside bounded steps (stop_core.h). Logging stays off until Start logging or a restart. ---- */
static stop_ctl_t s_stop;
static bool s_close_ok, s_rev_flushed, s_turnoff;
static int64_t s_ready_logged;

static void stop_apply(unsigned act)
{
    if ((act & STOP_DO_HALT) && !elm_user_stopped()) elm_user_stop(true);
    if (act & STOP_DO_CLOSE) {
        if (R.on) { status_get(&s_st); rec_finish(); }   /* the summary and the drives.csv line first */
        if (!s_rev_flushed) { rev_flush(3000); s_rev_flushed = true; }   /* the learned fuel state of this drive */
        s_close_ok = sdlog_stop_close(500);
    }
    if (act & STOP_DO_PAUSE) sdlog_writers_hold(true);
    if (act & STOP_DO_SYNC) sdlog_sync_now();
    if (act & STOP_DO_RELEASE) {
        sdlog_writers_hold(false);
        obd_ble_set_paused(false);
        elm_user_stop(false);              /* the link may start the next drive (its own session) */
    }
}

static void stop_tick(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (s_start_req) {
        s_start_req = false;
        if (s_stop.latched) {
            stop_apply(stop_release(&s_stop, now));
            s_turnoff = false;
            sys_event(EV_INFO, "Logging started again");
            ESP_LOGI(TAG, "stop: logging started again by the user");
        }
    }
    if (s_end_req) {
        s_end_req = false;
        stop_state_t was = s_stop.st;
        unsigned act = stop_request(&s_stop, now);
        if (s_turnoff_req) { s_turnoff = true; s_turnoff_req = false; }
        if (was == STOP_IDLE) {
            s_close_ok = false;
            s_rev_flushed = false;
            sys_event(EV_INFO, s_turnoff ? "Turn off pressed: stopping logging" : "Stop logging pressed: saving the drive");
            ESP_LOGI(TAG, "stop: requested (%s), drive %s", s_turnoff ? "turn off" : "stop logging", elm_drive_active() ? "running" : "not running");
        }
        stop_apply(act);
    }
    if (s_retry_req) { s_retry_req = false; s_close_ok = false; stop_apply(stop_retry(&s_stop, now)); }
    if (s_stop.st == STOP_IDLE) return;
    stop_state_t before = s_stop.st;
    stop_in_t in = {
        .poll_running = elm_drive_active(),
        .session_open = sdlog_session_open(),
        .closed_ok = s_close_ok || !sdlog_session_open(),
        .saved = sdlog_saved() || !sdlog_session_open(),
        .ms_since_write = sdlog_ms_since_any_write(),
        .writers_paused = sdlog_writers_idle(),
        .storage_ok = sdlog_ok(),
    };
    stop_apply(stop_step(&s_stop, &in, now));
    if (s_stop.st != before) {
        ESP_LOGI(TAG, "stop: %s after %lld ms%s%s", stop_state_text(&s_stop), (long long)(now - s_stop.t_req),
                 s_stop.st == STOP_FAILED ? ": " : "", s_stop.st == STOP_FAILED ? stop_why_text(s_stop.why) : "");
        if (s_stop.st == STOP_READY && s_ready_logged != s_stop.t_req) {
            s_ready_logged = s_stop.t_req;
            sys_event(EV_INFO, "Logging stopped: safe to unplug");
            s_auto_open = !s_turnoff && s_pub.ok;          /* the drive summary, as after End drive before */
            if (s_turnoff) obd_ble_set_paused(true);       /* Turn off: no adapter search while it waits for the unplug */
        }
        if (s_stop.st == STOP_FAILED) sys_event(EV_ERROR, "Stop failed: %s", stop_why_text(s_stop.why));
    }
}

static void drive_task(void *arg)
{
    (void)arg;
    history_load();
    if (s_hist.n) past_load(s_hist.d[s_hist.n - 1].session);
    int64_t last_rec = 0;
    stop_init(&s_stop);
    for (;;) {
        /* 100 ms steps while a stop runs (its progress on the screen), the 1 s recording cadence otherwise */
        vTaskDelay(pdMS_TO_TICKS(stop_active(&s_stop) || s_end_req || s_start_req || s_retry_req ? 100 : 250));
        s_ending = stop_active(&s_stop);
        stop_tick();
        s_ending = stop_active(&s_stop);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_rec < 1000) continue;
        last_rec = now_ms;
        if (s_hist_req) { s_hist_req = false; history_load(); }
        bool active = elm_drive_active();
        status_get(&s_st);
        if (active && !R.on) rec_start();
        if (R.on && active) { rec_sample(); rec_events(); }
        if (R.on && !active) rec_finish();              /* the car ended the drive (ignition off, link lost) */
    }
}

void mosaico_drive_start(void)
{
    s_mx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(drive_task, "drive_rec", 6144, NULL, 1, NULL, 0);   /* 2.1.1: the stop closes and syncs files here */
}

void mosaico_drive_end_request(void) { s_end_req = true; }
void mosaico_drive_turn_off_request(void) { s_turnoff_req = true; s_end_req = true; }
void mosaico_drive_stop_retry(void) { s_retry_req = true; }
void mosaico_drive_start_request(void) { s_start_req = true; }
bool mosaico_drive_ending(void) { return s_ending || s_end_req; }
bool mosaico_drive_stopped(void) { return s_stop.latched && !s_ending && !s_end_req; }

void mosaico_drive_stop_get(md_stop_t *out)
{
    memset(out, 0, sizeof *out);
    stop_ctl_t c = s_stop;                 /* a byte-wise copy of a small struct: a torn read only delays one frame */
    out->state = (uint8_t)c.st;
    out->turn_off = s_turnoff;
    out->requested = s_end_req;
    int64_t now = esp_timer_get_time() / 1000;
    out->secs = c.t_req ? (uint32_t)(((c.t_done ? c.t_done : now) - c.t_req) / 1000) : 0;
    out->since_done_ms = c.t_done ? (uint32_t)(now - c.t_done) : 0;
    snprintf(out->text, sizeof out->text, "%s", stop_state_text(&c));
    snprintf(out->why, sizeof out->why, "%s", stop_why_text(c.why));
}
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
