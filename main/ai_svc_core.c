/* ai_svc_core: see ai_svc_core.h */
#include "ai_svc_core.h"

void ai_rt_start(ai_rt_t *t, int64_t now_us)
{
    t->next_us = now_us;
    t->release_us = now_us;
}

uint64_t ai_rt_period_us(const ai_rt_t *t, uint8_t level)
{
    return (uint64_t)t->period_ms * 1000u * (level == AI_DL_SLOW ? (t->slow_factor ? t->slow_factor : AI_DL_SLOW_FACTOR) : 1);
}

int ai_rt_pick(ai_rt_t *t, int n, int64_t now_us, uint8_t level, int64_t *wake_us)
{
    int best = -1;
    int64_t wake = now_us + 1000000;
    for (int i = 0; i < n; i++) {
        ai_rt_t *s = &t[i];
        if (!s->active) continue;
        int64_t p = (int64_t)ai_rt_period_us(s, level);
        if (level == AI_DL_PAUSED) {                     /* no release while paused, and no backlog when it resumes */
            if (s->next_us < now_us + p) s->next_us = now_us + p;
            continue;
        }
        if (s->next_us > now_us) { if (s->next_us < wake) wake = s->next_us; continue; }
        if (best < 0 || s->deadline_ms < t[best].deadline_ms) best = i;
    }
    if (best >= 0) {
        ai_rt_t *s = &t[best];
        int64_t p = (int64_t)ai_rt_period_us(s, level);
        int64_t late = (now_us - s->next_us) / p;       /* releases passed over: never queued */
        if (late > 0) { s->skipped += (uint32_t)late; s->next_us += late * p; }
        s->release_us = s->next_us;
        s->next_us += p;
        s->released++;
        wake = now_us;
    }
    *wake_us = wake;
    return best;
}

void ai_rt_finish(ai_rt_t *t, int64_t start_us, int64_t end_us, uint8_t level)
{
    uint32_t resp = (uint32_t)(end_us - t->release_us), step = (uint32_t)(end_us - start_us);
    t->done++;
    t->last_resp_us = resp;
    if (resp > t->wcrt_us) t->wcrt_us = resp;
    if (step > t->max_step_us) t->max_step_us = step;
    if (resp > t->deadline_ms * 1000u) t->missed++;
    if (t->budget_us && step > t->budget_us) {          /* overrun: it finished, the next release is skipped */
        t->overruns++;
        t->skipped++;
        t->next_us += (int64_t)ai_rt_period_us(t, level);
    }
}

ai_selftest_t ai_selftest_judge(const uint32_t *runs, int k, uint32_t budget_us)
{
    ai_selftest_t r = { UINT32_MAX, UINT32_MAX, 0, false };
    uint32_t t[16];
    if (k <= 0) return r;
    if (k > 16) k = 16;
    for (int i = 0; i < k; i++) {
        t[i] = runs[i];
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { uint32_t u = t[j]; t[j] = t[j - 1]; t[j - 1] = u; }
    }
    r.min_us = t[0];
    r.median_us = t[k / 2];
    r.max_us = t[k - 1];
    bool failed = false;                            /* a failed check (UINT32_MAX) in any run is a real failure */
    for (int i = 0; i < k; i++) if (runs[i] == UINT32_MAX) failed = true;
    r.ok = !failed && (!budget_us || r.min_us <= budget_us);
    return r;
}

uint8_t ai_dl_from_os(uint8_t os_level)
{
    return os_level == 0 ? AI_DL_NORMAL : os_level == 1 ? AI_DL_SLOW : AI_DL_PAUSED;
}

const char *ai_dl_name(uint8_t level)
{
    static const char *const N[3] = { "normal", "slowed x10", "paused" };
    return level < 3 ? N[level] : "?";
}

void ai_rt_worker_load(const ai_rt_t *t, int n, uint32_t *c_us, uint32_t *t_ms, uint32_t *d_ms)
{
    uint32_t c = 0, tp = 0, dl = 0;
    for (int i = 0; i < n; i++) {
        if (!t[i].active) continue;
        c += t[i].budget_us;
        if (!tp || t[i].period_ms < tp) tp = t[i].period_ms;
        if (!dl || t[i].deadline_ms < dl) dl = t[i].deadline_ms;
    }
    *c_us = c; *t_ms = tp; *d_ms = dl;
}
