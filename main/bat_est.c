/* bat_est: see bat_est.h. Worker side: step() (never blocks). Board-task side: prepare(), board(). */
#include "bat_est.h"
#if CONFIG_OCM_BAT_EST
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ai_snap.h"
#include "bat_est_core.h"
#if CONFIG_OCM_BAT_LOG
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "os.h"
#include "sdlog.h"
#endif

static const char *TAG = "bat";

#define BAT_PERIOD_MS    1000
#define BAT_DEADLINE_MS  1000
#define BAT_BUDGET_US    2000                  /* one step plus a log row; the self-test is checked against it */
#define BAT_ROW_US       10000000              /* a log row every 10 s */
#define BAT_SAVE_US      (600LL * 1000000)     /* NVS at most every 10 minutes */
#define BAT_NOMINAL_MAH  ((float)CONFIG_OCM_BAT_NOMINAL_MAH)   /* UNVERIFIED: the cell's label */

/* ---------- worker side: written by the worker only ---------- */
static bat_est_t s_e;
static bool s_init;
static uint32_t s_seen;
static int64_t s_t_last, s_next_row;

static bat_est_out_t s_out;
static ai_seq_t s_out_seq;
static bat_persist_t s_per;                   /* the newest values worth keeping */
static ai_seq_t s_per_seq;
static volatile uint32_t s_per_gen;

#if CONFIG_OCM_BAT_LOG
EXT_RAM_BSS_ATTR static ai_ring_t s_ring;     /* worker -> board task */
#endif
static volatile uint32_t s_log_rows, s_log_skipped;   /* board task */

/* ---------- board-task side ---------- */
static bat_persist_t s_saved;                 /* prepare() fills it before the worker exists */
static bool s_saved_ok;

static const char *const MODE_TOKEN[4] = { "rest", "discharge", "charge", "cv" };

/* the training row: the gauge sample, the filter's inputs and outputs */
static int format_row(char *b, size_t n, const ai_gauge_sample_t *gs, float i_avg_a, const bat_est_t *e)
{
    return snprintf(b, n, "%lu,%lld,%d,%d,%.1f,%.1f,%d,%d,%d,%.4f,%.4f,%.4f,%.1f,%.3f,%s,%u\n",
                    (unsigned long)(gs->t_us / 1000000), (long long)time(NULL), gs->g.mv, gs->g.ma, (double)(i_avg_a * 1000.0f),
                    gs->g.temp_dc / 10.0, gs->g.soc, gs->g.rem_mah, gs->g.full_mah, (double)e->z_ekf, (double)e->z_corr,
                    (double)e->soc_user, (double)bat_soh_pct(e), (double)bat_dcir_ratio(e), MODE_TOKEN[e->mode & 3],
                    (unsigned)((e->anchored ? 1u : 0u) | (bat_model_placeholder() ? 2u : 0u) | (e->ridge_off ? 4u : 0u)));
}

static void publish(float i_avg_a)
{
    bat_est_out_t o = {
        .valid = true, .anchored = s_e.anchored, .placeholder = bat_model_placeholder(), .mode = s_e.mode,
        .soc_pct = 100.0f * s_e.soc_user, .soh_pct = bat_soh_pct(&s_e), .dcir_ratio = bat_dcir_ratio(&s_e),
        .iavg_ma = i_avg_a * 1000.0f, .steps = s_e.steps,
    };
    AI_SEQ_PUT(&s_out_seq, &s_out, &o);
    bat_persist_t p;
    bat_est_persist(&s_e, &p);
    AI_SEQ_PUT(&s_per_seq, &s_per, &p);
    s_per_gen++;
}

bool bat_est_read(bat_est_out_t *out)
{
    if (!AI_SEQ_GET(&s_out_seq, out, &s_out) || !out->valid) return false;
    out->log_rows = s_log_rows;
    out->log_skipped = s_log_skipped;
#if CONFIG_OCM_BAT_LOG
    out->log_dropped = s_ring.dropped;
#endif
    return true;
}

static void step(int64_t release_us)
{
    static float i_last;
    ai_gauge_sample_t gs;
    if (!ai_svc_gauge_read(&gs) || !gs.g.ok || gs.n == s_seen) return;      /* a new sample every 2 s */
    /* Current(): positive = charging (TI convention, UNVERIFIED on this board); the mean over the sample is the trapezoid
       of two readings (AverageCurrent() would be a new I2C read, which the owner ruled out) */
    float v = gs.g.mv / 1000.0f, i = gs.g.ma / 1000.0f, i_avg = 0.5f * (i_last + i);
    int valid = gs.g.mv >= 2500 && gs.g.mv <= 4600;                          /* else a glitch, not a cell voltage */
    if (!s_init) {
        if (!valid) { s_seen = gs.n; return; }
        bat_est_boot(&s_e, s_saved_ok ? &s_saved : NULL, v, i);
        s_init = true;
        s_next_row = release_us;
        i_avg = i;
    } else {
        s_e.ridge_off = ai_svc_level() != AI_DL_NORMAL;   /* the ladder's AI rung: the EKF alone, no learned head */
        bat_in_t in = { v, i, i_avg, gs.g.temp_dc / 10.0f, (float)((gs.t_us - s_t_last) / 1e6), valid };
        bat_est_step(&s_e, &in);
    }
    i_last = i;
    s_seen = gs.n;
    s_t_last = gs.t_us;
    publish(i_avg);
#if CONFIG_OCM_BAT_LOG
    if (release_us >= s_next_row) {
        s_next_row = release_us + BAT_ROW_US;
        char row[AI_RING_LINE];
        format_row(row, sizeof row, &gs, i_avg, &s_e);
        ai_ring_push(&s_ring, row);                     /* full: dropped and counted, never waited for */
    }
#else
    (void)release_us;
#endif
}

/* one step and one row on fixed data, ridge on (the worst case), the slower of three (us) */
static uint32_t selftest(void)
{
    EXT_RAM_BSS_ATTR static bat_est_t e;                /* board task: off its stack */
    ai_gauge_sample_t gs = { .g = { .ok = true, .mv = 3900, .ma = -25, .soc = 60, .temp_dc = 250 }, .t_us = 0, .n = 1 };
    bat_est_init(&e, 0.6f, bat_model_p0_z(), bat_model_p0_v(), 1.0f, true);
    uint32_t worst = 0;
    char row[AI_RING_LINE];
    for (int k = 0; k < 3; k++) {
        bat_in_t in = { 3.9f - 0.001f * k, -0.025f - 0.02f * k, -0.025f, 25.0f, 2.0f, 1 };
        int64_t t0 = esp_timer_get_time();
        bat_est_step(&e, &in);
        format_row(row, sizeof row, &gs, in.i_avg, &e);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > worst) worst = us;
    }
    return worst;
}

static void prepare(void)
{
    bat_model_set_qnom_ah(BAT_NOMINAL_MAH / 1000.0f);
    nvs_handle_t h;
    size_t n = sizeof s_saved;
    s_saved_ok = false;
    if (nvs_open("ocm_bat", NVS_READONLY, &h) == ESP_OK) {
        s_saved_ok = nvs_get_blob(h, "est", &s_saved, &n) == ESP_OK && n == sizeof s_saved && bat_persist_valid(&s_saved);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "estimator: model %s%s, nominal %d mAh (UNVERIFIED), %s", bat_model_id(),
             bat_model_placeholder() ? " (PLACEHOLDER)" : "", CONFIG_OCM_BAT_NOMINAL_MAH,
             s_saved_ok ? "learnt values restored from NVS" : "no saved values: SoC from the first OCV");
    if (s_saved_ok)
        ESP_LOGI(TAG, "restored: SoC %.1f %%, SoH %.1f %% (%lu samples), DCIR %.1f mV/C (%lu samples)%s", s_saved.z * 100.0,
                 s_saved.soh * 100.0, (unsigned long)s_saved.n_q, s_saved.dcir * 1000.0, (unsigned long)s_saved.n_dcir,
                 (s_saved.flags & 1) ? "" : ", not anchored yet");
}

/* ---------- NVS: at most every 10 minutes, and on the safe-shutdown path ---------- */
static void persist(bool shutdown)
{
    static int64_t last_us = -1;
    static uint32_t written_gen;
    int64_t now = esp_timer_get_time();
    if (last_us < 0) last_us = now;                      /* the first write 10 minutes after the start */
    uint32_t gen = s_per_gen;
    if (gen == written_gen || (!shutdown && now - last_us < BAT_SAVE_US)) return;
    bat_persist_t p;
    if (!AI_SEQ_GET(&s_per_seq, &p, &s_per) || p.magic != BAT_PERSIST_MAGIC) return;
    nvs_handle_t h;
    esp_err_t e = nvs_open("ocm_bat", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "est", &p, sizeof p);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    last_us = now;
    written_gen = gen;
    ESP_LOGI(TAG, "learnt values saved%s: SoC %.1f %%, SoH %.1f %%, DCIR %.1f mV/C: %s", shutdown ? " (shutdown)" : "",
             p.z * 100.0, p.soh * 100.0, p.dcir * 1000.0, esp_err_to_name(e));
}

/* ---------- the battery log (board task) ---------- */
#if CONFIG_OCM_BAT_LOG
#define LOG_CAP   ((uint32_t)CONFIG_OCM_BAT_LOG_CAP_KB * 1024u)
#define FILE_CAP  (LOG_CAP / 8u)
static const char LOG_HEADER[] =
    "uptime_s,epoch_s,v_mv,i_ma,iavg_ma,temp_c,gauge_soc,gauge_rem_mah,gauge_full_mah,z_ekf,z_corr,soc_user,est_soh,dcir_ratio,mode,flags\n";
static long s_lo = -1, s_hi = -1;
static uint32_t s_total, s_cur;

bool bat_log_name_valid(const char *n)
{
    if (!n || strlen(n) != 11 || n[0] != 'b') return false;
    for (int i = 1; i <= 6; i++) if (n[i] < '0' || n[i] > '9') return false;
    return !strcmp(n + 7, ".csv");
}

static void log_path(char *b, size_t n, long k) { snprintf(b, n, BAT_LOG_DIR "/b%06ld.csv", k); }

static void log_scan(void)
{
    mkdir(BAT_LOG_DIR, 0775);
    DIR *d = opendir(BAT_LOG_DIR);
    s_lo = s_hi = -1;
    s_total = s_cur = 0;
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        if (!bat_log_name_valid(e->d_name)) continue;
        long k = atol(e->d_name + 1);
        char p[64];
        struct stat st;
        log_path(p, sizeof p, k);
        if (stat(p, &st) != 0) continue;
        s_total += (uint32_t)st.st_size;
        if (s_lo < 0 || k < s_lo) s_lo = k;
        if (k > s_hi) { s_hi = k; s_cur = (uint32_t)st.st_size; }
    }
    if (d) closedir(d);
    ESP_LOGI(TAG, "battery log: %s, files %ld..%ld, %lu KB of %lu KB", d ? BAT_LOG_DIR : "no directory", s_lo, s_hi,
             (unsigned long)(s_total / 1024), (unsigned long)(LOG_CAP / 1024));
}

static void log_drain(bool shutdown)
{
    static bool scanned;
    static int64_t last_us;
    if (!ai_ring_peek(&s_ring)) return;
    int64_t now = esp_timer_get_time();
    if (!shutdown && now - last_us < (int64_t)CONFIG_SDLOG_SYNC_PERIOD_S * 1000000) return;   /* the drive logs' save period */
    last_us = now;
    /* busy: no NAND, storage quiesced for a reset, or a format / update running. The rows wait in the ring (dropped and
       counted by the worker when it is full). */
    if (!nand_store_mounted() || !sdlog_ok() || os_runlevel() == OS_RL_UPDATE) { s_log_skipped++; return; }
    mosaico_storage_t f;
    mosaico_storage_read(&f);
    if (f.ok && f.nand && f.free_kb < 2u * CONFIG_OCM_BAT_LOG_CAP_KB) {            /* nearly full: the drive logs first */
        while (ai_ring_peek(&s_ring)) { ai_ring_pop(&s_ring); s_log_skipped++; }
        return;
    }
    if (!scanned) { log_scan(); scanned = true; }
    bool fresh = s_hi < 0 || s_cur >= FILE_CAP;
    if (fresh) { s_hi = s_hi < 0 ? 1 : s_hi + 1; s_cur = 0; if (s_lo < 0) s_lo = s_hi; }
    char p[64];
    log_path(p, sizeof p, s_hi);
    FILE *fp = fopen(p, "a");
    if (!fp) { s_log_skipped++; return; }
    setvbuf(fp, NULL, _IONBF, 0);       /* no stdio buffer (internal RAM): FatFs's own sector buffer (PSRAM) collects the rows */
    if (fresh) { fputs(LOG_HEADER, fp); s_cur += sizeof LOG_HEADER - 1; s_total += sizeof LOG_HEADER - 1; }
    for (const char *l; (l = ai_ring_peek(&s_ring)) != NULL; ai_ring_pop(&s_ring)) {
        size_t n = strlen(l);
        if (fputs(l, fp) < 0) break;
        s_cur += (uint32_t)n;
        s_total += (uint32_t)n;
        s_log_rows++;
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    while (s_total > LOG_CAP && s_lo >= 0 && s_lo < s_hi) {                         /* the oldest file goes first */
        struct stat st;
        log_path(p, sizeof p, s_lo);
        if (stat(p, &st) == 0) { s_total -= (uint32_t)st.st_size < s_total ? (uint32_t)st.st_size : s_total; remove(p); }
        s_lo++;
    }
}
#endif

static void board(bool shutdown)
{
    persist(shutdown);
#if CONFIG_OCM_BAT_LOG
    log_drain(shutdown);
#endif
}

static const ai_svc_def_t SVC = {
    .name = "bat", .period_ms = BAT_PERIOD_MS, .deadline_ms = BAT_DEADLINE_MS, .budget_us = BAT_BUDGET_US,
    .step = step, .selftest = selftest, .prepare = prepare, .board = board,
};

const ai_svc_def_t *bat_est_service(void) { return &SVC; }
#endif
