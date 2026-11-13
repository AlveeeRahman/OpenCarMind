/* pattern_model_load_json (pattern.h), ESP-Mosaico only (main/CMakeLists.txt), so the S3 image keeps pattern.c as it is.
   The same layout as pattern_model_to_json writes, parsed straight into the model. */
#include <string.h>
#include <stdlib.h>
#include "pattern.h"

/* pattern.c's num_array: a [n, n, ...] list of numbers, at most max kept, the end of the list returned */
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
    if (load_json_into(m, json)) return true;      /* load_json_into began with pattern_init: the window is reset too */
    pattern_init(m);
    return false;
}
