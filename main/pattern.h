/* Stage-1 unsupervised pattern layer, pure C (host-tested).
 * Every window (default 10 s) of driving becomes a standardised feature vector; an online
 * k-means assigns it to one of PATTERN_K centroids ("pattern id") and reports the distance as a
 * novelty score. SAX strings give a symbolic shape of rpm and speed inside the window. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PATTERN_K          24
#define PATTERN_SIGNALS    10
#define PATTERN_FEATURES   (PATTERN_SIGNALS * 3)   /* mean, std, slope per signal */
#define PATTERN_SAX_LEN    5
#define PATTERN_WINDOW_MS  10000
#define PATTERN_MAX_SAMPLES 128

typedef struct {
    uint32_t window_seq;
    int64_t  start_ms, end_ms;
    uint16_t samples;
    int16_t  pattern_id;        /* -1 until the first window */
    float    dist;              /* standardised Euclidean distance to the centroid */
    float    novelty;           /* dist / running mean dist (1.0 = typical) */
    bool     spawned;           /* this window created a new centroid */
    char     sax_rpm[PATTERN_SAX_LEN + 1];
    char     sax_speed[PATTERN_SAX_LEN + 1];
    float    features[PATTERN_FEATURES];
} pattern_result_t;

typedef struct {
    float    centroid[PATTERN_K][PATTERN_FEATURES];
    uint32_t count[PATTERN_K];
    uint8_t  used;
    /* running standardisation (Welford) per feature */
    double   mean[PATTERN_FEATURES], m2[PATTERN_FEATURES];
    uint32_t n_windows;
    double   mean_dist;
    float    spawn_threshold;   /* standardised distance above which a new centroid is created */
} pattern_model_t;

void pattern_init(pattern_model_t *m);
/* Feed one sample. signals[] order: rpm, speed, load, maf, throttle, stft, ltft, coolant, iat, map.
 * valid bitmask marks which signals are present. Returns true when a window closed; out is filled. */
bool pattern_feed(pattern_model_t *m, int64_t now_ms, const float signals[PATTERN_SIGNALS], uint16_t valid, pattern_result_t *out);
/* Serialise / parse the model as JSON text (centroids, counts, stats). */
int  pattern_model_to_json(const pattern_model_t *m, char *buf, size_t n);
bool pattern_model_from_json(pattern_model_t *m, const char *json);
const char *pattern_signal_name(int i);
