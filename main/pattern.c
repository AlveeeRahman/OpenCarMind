#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pattern.h"

static const char *SIGNAL_NAMES[PATTERN_SIGNALS] = {"rpm", "speed", "load", "maf", "throttle", "stft", "ltft", "coolant", "iat", "map"};
const char *pattern_signal_name(int i) { return (i >= 0 && i < PATTERN_SIGNALS) ? SIGNAL_NAMES[i] : "?"; }

/* window accumulator */
static struct {
    int64_t start_ms;
    uint16_t n;
    double sum[PATTERN_SIGNALS], sum2[PATTERN_SIGNALS];
    float first[PATTERN_SIGNALS], last[PATTERN_SIGNALS];
    int64_t first_ms[PATTERN_SIGNALS], last_ms[PATTERN_SIGNALS];
    uint16_t cnt[PATTERN_SIGNALS];
    float rpm[PATTERN_MAX_SAMPLES], speed[PATTERN_MAX_SAMPLES];
    uint16_t nrpm, nspeed;
    uint32_t seq;
} w;

void pattern_init(pattern_model_t *m)
{
    memset(m, 0, sizeof *m);
    m->spawn_threshold = 2.5f;
    memset(&w, 0, sizeof w);
}

static void reset_window(int64_t now_ms)
{
    uint32_t seq = w.seq;
    memset(&w, 0, sizeof w);
    w.seq = seq;
    w.start_ms = now_ms;
}

/* SAX: piecewise-aggregate approximation to PATTERN_SAX_LEN segments, then 4-letter alphabet
 * on the window-normalised series (breakpoints of the standard normal for 4 symbols). */
static void sax(const float *x, int n, char *out)
{
    if (n < 2) { strcpy(out, "-----"); return; }
    double mean = 0, var = 0;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;
    for (int i = 0; i < n; i++) var += (x[i] - mean) * (x[i] - mean);
    double sd = sqrt(var / n);
    for (int s = 0; s < PATTERN_SAX_LEN; s++) {
        int a = s * n / PATTERN_SAX_LEN, b = (s + 1) * n / PATTERN_SAX_LEN;
        if (b <= a) b = a + 1;
        double seg = 0;
        for (int i = a; i < b && i < n; i++) seg += x[i];
        seg /= (b - a);
        double z = sd > 1e-6 ? (seg - mean) / sd : 0;
        out[s] = z < -0.67 ? 'a' : z < 0 ? 'b' : z < 0.67 ? 'c' : 'd';
    }
    out[PATTERN_SAX_LEN] = 0;
}

static float std_feature(const pattern_model_t *m, int f, float raw)
{
    double sd = m->n_windows > 1 ? sqrt(m->m2[f] / (m->n_windows - 1)) : 0;
    if (sd < 1e-6) return 0;
    return (float)((raw - m->mean[f]) / sd);
}

bool pattern_feed(pattern_model_t *m, int64_t now_ms, const float signals[PATTERN_SIGNALS], uint16_t valid, pattern_result_t *out)
{
    if (w.start_ms == 0) w.start_ms = now_ms;
    for (int i = 0; i < PATTERN_SIGNALS; i++) {
        if (!(valid & (1 << i))) continue;
        float v = signals[i];
        if (w.cnt[i] == 0) { w.first[i] = v; w.first_ms[i] = now_ms; }
        w.last[i] = v; w.last_ms[i] = now_ms;
        w.sum[i] += v; w.sum2[i] += (double)v * v; w.cnt[i]++;
    }
    if ((valid & 1) && w.nrpm < PATTERN_MAX_SAMPLES) w.rpm[w.nrpm++] = signals[0];
    if ((valid & 2) && w.nspeed < PATTERN_MAX_SAMPLES) w.speed[w.nspeed++] = signals[1];
    w.n++;

    if (now_ms - w.start_ms < PATTERN_WINDOW_MS) return false;
    if (w.n < 3) { reset_window(now_ms); return false; }

    /* ---- raw features ---- */
    float raw[PATTERN_FEATURES];
    for (int i = 0; i < PATTERN_SIGNALS; i++) {
        float mean = 0, sd = 0, slope = 0;
        if (w.cnt[i] > 0) {
            mean = (float)(w.sum[i] / w.cnt[i]);
            double var = w.sum2[i] / w.cnt[i] - (double)mean * mean;
            sd = var > 0 ? (float)sqrt(var) : 0;
            double dt = (w.last_ms[i] - w.first_ms[i]) / 1000.0;
            slope = dt > 0.5 ? (float)((w.last[i] - w.first[i]) / dt) : 0;
        }
        raw[i * 3] = mean; raw[i * 3 + 1] = sd; raw[i * 3 + 2] = slope;
    }

    /* ---- update running standardisation, then standardise ---- */
    m->n_windows++;
    float z[PATTERN_FEATURES];
    for (int f = 0; f < PATTERN_FEATURES; f++) {
        double delta = raw[f] - m->mean[f];
        m->mean[f] += delta / m->n_windows;
        m->m2[f] += delta * (raw[f] - m->mean[f]);
        z[f] = std_feature(m, f, raw[f]);
    }

    /* ---- online k-means ---- */
    int best = -1;
    float best_d = INFINITY;
    for (int k = 0; k < m->used; k++) {
        float d = 0;
        for (int f = 0; f < PATTERN_FEATURES; f++) { float e = z[f] - m->centroid[k][f]; d += e * e; }
        d = sqrtf(d);
        if (d < best_d) { best_d = d; best = k; }
    }
    bool spawned = false;
    if (best < 0 || (best_d > m->spawn_threshold && m->used < PATTERN_K && m->n_windows > 5)) {
        best = m->used++;
        memcpy(m->centroid[best], z, sizeof z);
        m->count[best] = 1;
        best_d = 0;
        spawned = true;
    } else {
        m->count[best]++;
        float lr = 1.0f / m->count[best];
        if (lr < 0.05f) lr = 0.05f;
        for (int f = 0; f < PATTERN_FEATURES; f++) m->centroid[best][f] += lr * (z[f] - m->centroid[best][f]);
    }
    m->mean_dist += (best_d - m->mean_dist) / (m->n_windows < 50 ? m->n_windows : 50);

    /* ---- result ---- */
    memset(out, 0, sizeof *out);
    out->window_seq = ++w.seq;
    out->start_ms = w.start_ms;
    out->end_ms = now_ms;
    out->samples = w.n;
    out->pattern_id = (int16_t)best;
    out->dist = best_d;
    out->novelty = m->mean_dist > 1e-3 ? best_d / (float)m->mean_dist : (spawned ? 2.0f : 1.0f);
    out->spawned = spawned;
    sax(w.rpm, w.nrpm, out->sax_rpm);
    sax(w.speed, w.nspeed, out->sax_speed);
    memcpy(out->features, raw, sizeof raw);
    reset_window(now_ms);
    return true;
}

int pattern_model_to_json(const pattern_model_t *m, char *buf, size_t n)
{
    int o = snprintf(buf, n, "{\"k\":%d,\"features\":%d,\"used\":%u,\"n_windows\":%lu,\"mean_dist\":%.4f,\"spawn_threshold\":%.2f,\"signals\":[",
                     PATTERN_K, PATTERN_FEATURES, m->used, (unsigned long)m->n_windows, m->mean_dist, m->spawn_threshold);
    for (int i = 0; i < PATTERN_SIGNALS && o < (int)n; i++) o += snprintf(buf + o, n - o, "\"%s\"%s", SIGNAL_NAMES[i], i + 1 < PATTERN_SIGNALS ? "," : "");
    o += snprintf(buf + o, n - o, "],\"mean\":[");
    for (int f = 0; f < PATTERN_FEATURES && o < (int)n; f++) o += snprintf(buf + o, n - o, "%.5g%s", m->mean[f], f + 1 < PATTERN_FEATURES ? "," : "");
    o += snprintf(buf + o, n - o, "],\"m2\":[");
    for (int f = 0; f < PATTERN_FEATURES && o < (int)n; f++) o += snprintf(buf + o, n - o, "%.6g%s", m->m2[f], f + 1 < PATTERN_FEATURES ? "," : "");
    o += snprintf(buf + o, n - o, "],\"count\":[");
    for (int k = 0; k < m->used && o < (int)n; k++) o += snprintf(buf + o, n - o, "%lu%s", (unsigned long)m->count[k], k + 1 < m->used ? "," : "");
    o += snprintf(buf + o, n - o, "],\"centroids\":[");
    for (int k = 0; k < m->used && o < (int)n; k++) {
        o += snprintf(buf + o, n - o, "[");
        for (int f = 0; f < PATTERN_FEATURES && o < (int)n; f++) o += snprintf(buf + o, n - o, "%.4g%s", m->centroid[k][f], f + 1 < PATTERN_FEATURES ? "," : "");
        o += snprintf(buf + o, n - o, "]%s", k + 1 < m->used ? "," : "");
    }
    o += snprintf(buf + o, n - o, "]}");
    return o < (int)n ? o : -1;
}

/* Minimal parser for the exact layout written above (no general JSON needed). */
static const char *num_array(const char *p, double *out, int max, int *got)
{
    *got = 0;
    p = strchr(p, '[');
    if (!p) return NULL;
    p++;
    while (*p && *p != ']') {
        if (*p == ',' || *p == ' ') { p++; continue; }
        char *end;
        double v = strtod(p, &end);
        if (end == p) return NULL;
        if (*got < max) out[(*got)++] = v;
        p = end;
    }
    return *p == ']' ? p + 1 : NULL;
}

bool pattern_model_from_json(pattern_model_t *m, const char *json)
{
    pattern_model_t t;
    pattern_init(&t);
    const char *p;
    if (!(p = strstr(json, "\"used\":"))) return false;
    t.used = (uint8_t)strtoul(p + 7, NULL, 10);
    if (t.used > PATTERN_K) return false;
    if (!(p = strstr(json, "\"n_windows\":"))) return false;
    t.n_windows = (uint32_t)strtoul(p + 12, NULL, 10);
    if ((p = strstr(json, "\"mean_dist\":"))) t.mean_dist = strtod(p + 12, NULL);
    if ((p = strstr(json, "\"spawn_threshold\":"))) t.spawn_threshold = strtof(p + 18, NULL);
    double tmp[PATTERN_FEATURES];
    int got;
    if (!(p = strstr(json, "\"mean\":")) || !num_array(p, tmp, PATTERN_FEATURES, &got) || got != PATTERN_FEATURES) return false;
    for (int f = 0; f < PATTERN_FEATURES; f++) t.mean[f] = tmp[f];
    if (!(p = strstr(json, "\"m2\":")) || !num_array(p, tmp, PATTERN_FEATURES, &got) || got != PATTERN_FEATURES) return false;
    for (int f = 0; f < PATTERN_FEATURES; f++) t.m2[f] = tmp[f];
    if (!(p = strstr(json, "\"count\":"))) return false;
    double cnt[PATTERN_K];
    if (!num_array(p, cnt, PATTERN_K, &got) || got != t.used) return false;
    for (int k = 0; k < t.used; k++) t.count[k] = (uint32_t)cnt[k];
    if (!(p = strstr(json, "\"centroids\":"))) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;
    for (int k = 0; k < t.used; k++) {
        if (!(p = num_array(p, tmp, PATTERN_FEATURES, &got)) || got != PATTERN_FEATURES) return false;
        for (int f = 0; f < PATTERN_FEATURES; f++) t.centroid[k][f] = (float)tmp[f];
    }
    *m = t;
    memset(&w, 0, sizeof w);
    return true;
}

/* ESP-Mosaico field fix (2.1.1): pattern_model_from_json's local copy is 3.5 KB of stack, called from the obd task's first
   row of a boot. This parses the same layout in place, so only the small number buffers below are on the stack. */
static bool load_json_into(pattern_model_t *t, const char *json)
{
    pattern_init(t);
    const char *p;
    if (!(p = strstr(json, "\"used\":"))) return false;
    unsigned long used = strtoul(p + 7, NULL, 10);
    if (used > PATTERN_K) return false;
    t->used = (uint8_t)used;
    if (!(p = strstr(json, "\"n_windows\":"))) return false;
    t->n_windows = (uint32_t)strtoul(p + 12, NULL, 10);
    if ((p = strstr(json, "\"mean_dist\":"))) t->mean_dist = strtod(p + 12, NULL);
    if ((p = strstr(json, "\"spawn_threshold\":"))) t->spawn_threshold = strtof(p + 18, NULL);
    double tmp[PATTERN_FEATURES];
    int got;
    if (!(p = strstr(json, "\"mean\":")) || !num_array(p, tmp, PATTERN_FEATURES, &got) || got != PATTERN_FEATURES) return false;
    for (int f = 0; f < PATTERN_FEATURES; f++) t->mean[f] = tmp[f];
    if (!(p = strstr(json, "\"m2\":")) || !num_array(p, tmp, PATTERN_FEATURES, &got) || got != PATTERN_FEATURES) return false;
    for (int f = 0; f < PATTERN_FEATURES; f++) t->m2[f] = tmp[f];
    if (!(p = strstr(json, "\"count\":"))) return false;
    double cnt[PATTERN_K];
    if (!num_array(p, cnt, PATTERN_K, &got) || got != t->used) return false;
    for (int k = 0; k < t->used; k++) t->count[k] = (uint32_t)cnt[k];
    if (!(p = strstr(json, "\"centroids\":"))) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;
    for (int k = 0; k < t->used; k++) {
        if (!(p = num_array(p, tmp, PATTERN_FEATURES, &got)) || got != PATTERN_FEATURES) return false;
        for (int f = 0; f < PATTERN_FEATURES; f++) t->centroid[k][f] = (float)tmp[f];
    }
    return true;
}

bool pattern_model_load_json(pattern_model_t *m, const char *json)
{
    if (load_json_into(m, json)) { memset(&w, 0, sizeof w); return true; }
    pattern_init(m);
    return false;
}
