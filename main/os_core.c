/* os_core.c: the pure rules of the mossREV OS layer. See os_core.h. */
#include "os_core.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifndef CONFIG_CARMIND_ML_STRIDE_S
#define CONFIG_CARMIND_ML_STRIDE_S 1                  /* host tests: the shipped value */
#endif
#include <stdio.h>
#include <string.h>

/* ---------- 1. resets ----------
   REBOOT and FORMAT: their callers (usb_door.c, sdlog.c, shared with the S3 build) already wait 300 ms, so pre_ms is 0.
   The door's 100 ms after the quiesce is the old mosaico_console_download() delay, kept. The stall guard and the
   monitor's restart are crash paths: no marker, so three of them in a row still reach safe mode. */
static const os_reset_plan_t PLANS[SYS_RESET_N] = {
    [SYS_RESET_REBOOT]    = { "REBOOT",    1, 1, OS_RST_RESTART,      0, 0 },
    [SYS_RESET_DOOR]      = { "door",      1, 1, OS_RST_ROM_DOWNLOAD, 0, 100000 },
    [SYS_RESET_FORMAT]    = { "format",    1, 1, OS_RST_RESTART,      0, 0 },
    [SYS_RESET_UI_STALL]  = { "ui stall",  0, 1, OS_RST_ABORT,        0, 0 },
    [SYS_RESET_MONITOR]   = { "monitor",   0, 1, OS_RST_RESTART,      0, 0 },
};

const os_reset_plan_t *os_reset_plan(sys_reset_reason_t r)
{
    return (unsigned)r < SYS_RESET_N ? &PLANS[r] : NULL;
}

int os_reset_steps(sys_reset_reason_t r, uint8_t steps[5])
{
    const os_reset_plan_t *p = os_reset_plan(r);
    int n = 0;
    if (!p) return 0;
    if (p->pre_ms) steps[n++] = OS_STEP_WAIT;
    if (p->asked_for) steps[n++] = OS_STEP_MARK;
    if (p->quiesce) steps[n++] = OS_STEP_QUIESCE;
    if (p->post_us) steps[n++] = OS_STEP_SETTLE;
    steps[n++] = OS_STEP_RESET;
    return n;
}

/* ---------- 2. runlevels ----------
   The masks. Boot refuses a model switch and a format (the model and the stores are still starting) and nothing else,
   so the model still loads (ML) and the learner still starts in Boot. Safe refuses the model, the learner and a model
   switch (that would load a model), never a format or the clock (recovery stays possible). Driving refuses everything
   the old drive checks refused. Saving and Update refuse every change. */
#define RL(r) (1u << (r))
#define ALL   (RL(OS_RL_BOOT) | RL(OS_RL_SAFE) | RL(OS_RL_PARKED) | RL(OS_RL_DRIVING) | RL(OS_RL_SAVING) | RL(OS_RL_UPDATE))
static const uint8_t MASK[OS_SVC_N] = {
    [OS_SVC_MODEL_SWITCH] = RL(OS_RL_PARKED),
    [OS_SVC_CLOCK_SET]    = RL(OS_RL_BOOT) | RL(OS_RL_SAFE) | RL(OS_RL_PARKED),
    [OS_SVC_FORMAT]       = RL(OS_RL_SAFE) | RL(OS_RL_PARKED),
    [OS_SVC_WIFI_EDIT]    = RL(OS_RL_BOOT) | RL(OS_RL_SAFE) | RL(OS_RL_PARKED),
    [OS_SVC_WIFI_RADIO]   = RL(OS_RL_BOOT) | RL(OS_RL_SAFE) | RL(OS_RL_PARKED),
    [OS_SVC_DOOR]         = RL(OS_RL_BOOT) | RL(OS_RL_SAFE) | RL(OS_RL_PARKED),
    [OS_SVC_ML]           = ALL & ~(RL(OS_RL_SAFE) | RL(OS_RL_UPDATE)),
    [OS_SVC_LEARNER]      = RL(OS_RL_BOOT) | RL(OS_RL_PARKED) | RL(OS_RL_DRIVING),
};
static const uint8_t ORDER[OS_RL_N] = { OS_RL_UPDATE, OS_RL_SAVING, OS_RL_DRIVING, OS_RL_SAFE, OS_RL_BOOT, OS_RL_PARKED };

uint8_t os_rl_active(const os_rl_in_t *in)
{
    uint8_t a = 0;
    if (in->boot) a |= RL(OS_RL_BOOT);
    if (in->safe) a |= RL(OS_RL_SAFE);
    if (in->drive || in->link) a |= RL(OS_RL_DRIVING);
    if (in->saving) a |= RL(OS_RL_SAVING);
    if (in->update) a |= RL(OS_RL_UPDATE);
    return a ? a : (uint8_t)RL(OS_RL_PARKED);
}

os_runlevel_t os_rl_primary(uint8_t active)
{
    for (int i = 0; i < OS_RL_N; i++) if (active & RL(ORDER[i])) return (os_runlevel_t)ORDER[i];
    return OS_RL_PARKED;
}

uint8_t os_svc_mask(os_svc_t svc) { return (unsigned)svc < OS_SVC_N ? MASK[svc] : 0; }

bool os_rl_allows(uint8_t active, os_svc_t svc)
{
    return (unsigned)svc < OS_SVC_N && active && (active & ~MASK[svc]) == 0;
}

os_runlevel_t os_rl_blocker(uint8_t active, os_svc_t svc)
{
    if ((unsigned)svc >= OS_SVC_N) return OS_RL_PARKED;
    for (int i = 0; i < OS_RL_N; i++) if ((active & RL(ORDER[i])) && !(MASK[svc] & RL(ORDER[i]))) return (os_runlevel_t)ORDER[i];
    return OS_RL_N;
}

const char *os_rl_name(os_runlevel_t r)
{
    static const char *const N[OS_RL_N] = { "Boot", "Safe", "Parked", "Driving", "Saving", "Update" };
    return (unsigned)r < OS_RL_N ? N[r] : "?";
}

const char *os_rl_phrase(os_runlevel_t r, const os_rl_in_t *in)
{
    switch (r) {
    case OS_RL_BOOT:    return "the board is starting";
    case OS_RL_SAFE:    return "safe mode is on";
    case OS_RL_DRIVING: return in && !in->drive && in->link ? "the car link is up" : "a drive is running";
    case OS_RL_SAVING:  return "the board is saving power";
    case OS_RL_UPDATE:  return "an update or format is running";
    default:            return "not allowed now";
    }
}

const char *os_svc_name(os_svc_t svc)
{
    static const char *const N[OS_SVC_N] = { "model switch", "clock", "format", "Wi-Fi edit", "Wi-Fi radio", "door", "model",
                                             "learner" };
    return (unsigned)svc < OS_SVC_N ? N[svc] : "?";
}

/* ---------- 3. boot health ---------- */
uint8_t os_health_verdict(const os_health_in_t *in, uint16_t *flags)
{
    uint16_t f = 0;
    if (in->model_expected && in->selftest == 0) f |= OS_HF_MODEL_DIFF;
    if (in->model_expected && in->selftest < 0) f |= OS_HF_MODEL_NOT_RUN;
    if (in->nor_repairs) f |= OS_HF_NOR_REPAIRED;
    if (in->nor_warn) f |= OS_HF_NOR_WARN;
    if (in->nor <= 0) f |= OS_HF_NOR_DOWN;
    if (in->nand == 0 || in->nand == -1) f |= OS_HF_NAND;
    if (!in->first_tick_ms) f |= OS_HF_NO_FRAME;
    else if (in->first_tick_ms > OS_H_SLOW_FRAME_MS) f |= OS_HF_SLOW_FRAME;
    if (!in->touch_ok) f |= OS_HF_TOUCH;
    if (in->misses) f |= OS_HF_MISSES;
    if (in->stalls) f |= OS_HF_STALLS;
    if (!in->schedulable) f |= OS_HF_UNSCHED;
    if (in->safe) f |= OS_HF_SAFE;
    if (flags) *flags = f;
    return f & OS_H_BAD_FLAGS ? OS_H_BAD : f ? OS_H_DEGRADED : OS_H_GOOD;
}

const char *os_health_name(uint8_t v)
{
    static const char *const N[4] = { "pending", "good", "degraded", "bad" };
    return v < 4 ? N[v] : "?";
}

int os_health_text(uint8_t verdict, uint16_t flags, char *buf, size_t n)
{
    static const char *const W[13] = { "model self-test differs", "model self-test not run", "flash FAT repaired",
        "flash FAT audit warning", "flash FAT not mounted", "NAND not mounted", "no UI tick", "UI slow to start",
        "touch failing", "deadline misses", "job stalls", "not schedulable", "safe mode" };
    if (!buf || !n) return 0;
    int o = snprintf(buf, n, "%s", os_health_name(verdict)), first = 1;
    for (int i = 0; i < 13 && (size_t)o < n; i++) {
        if (!(flags & (1u << i))) continue;
        o += snprintf(buf + o, n - (size_t)o, "%s%s", first ? ": " : ", ", W[i]);
        first = 0;
    }
    return o < (int)n ? o : (int)n - 1;
}

/* ---------- 4. response-time analysis and model admission ---------- */
int os_rta(const os_rt_task_t *t, int n, int core, uint32_t *r_us)
{
    int misses = 0;
    for (int i = 0; i < n; i++) {
        r_us[i] = 0;
        if (t[i].core != core) continue;
        uint64_t r = t[i].c_us, prev = UINT64_MAX;
        int it = 0;
        while (r != prev && r <= t[i].d_us && it++ < 1000) {
            prev = r;
            r = t[i].c_us;
            for (int j = 0; j < n; j++) {
                if (j == i || t[j].core != core || t[j].prio <= t[i].prio || !t[j].t_us) continue;
                r += ((prev + t[j].t_us - 1) / t[j].t_us) * t[j].c_us;      /* ceil(prev / T_j) releases of j */
            }
        }
        bool ok = r == prev && r <= t[i].d_us;
        r_us[i] = ok ? (uint32_t)r : OS_RTA_MISS;
        misses += !ok;
    }
    return misses;
}

bool os_admit(const os_rt_task_t *set, int n, const os_rt_task_t *cand, uint32_t measured_us, uint32_t factor_pm, os_admit_t *out)
{
    os_rt_task_t t[OS_RTA_MAX];
    memset(out, 0, sizeof *out);
    if (n > OS_RTA_MAX - 1) n = OS_RTA_MAX - 1;
    memcpy(t, set, (size_t)n * sizeof t[0]);
    t[n] = *cand;
    uint64_t c = (uint64_t)measured_us * factor_pm / 1000u;
    t[n].c_us = c > UINT32_MAX ? UINT32_MAX : (uint32_t)c;
    out->c_us = t[n].c_us;
    out->n = n + 1;
    int miss = os_rta(t, n + 1, cand->core, out->r_us);
    int64_t best = INT64_MAX;
    out->worst = n;
    for (int i = 0; i <= n; i++) {
        if (t[i].core != cand->core) continue;
        int64_t m = out->r_us[i] == OS_RTA_MISS ? -(int64_t)t[i].d_us : (int64_t)t[i].d_us - out->r_us[i];
        if (m < best) { best = m; out->worst = i; }
    }
    out->margin_us = best < INT32_MIN ? INT32_MIN : best > INT32_MAX ? INT32_MAX : (int32_t)best;
    out->ok = miss == 0;
    return out->ok;
}

/* ---------- 5. AI service contract ----------
   The values are the ones the code registered before 2.0.0 (ml_infer.cpp, rev_task.c): the ml job runs once per model
   stride (1 s) with that deadline and a 10 s stall limit (a car paces rows at about 1.4 s with gaps over 3 s), priority 3
   on core 1. The learner pairs at most once per model run, its deadline is the commit cadence (30 s), it may stall 60 s,
   and it gets 2 % of a core in 30 ms bursts, priority 2 on core 1. */
#define ML_MS ((CONFIG_CARMIND_ML_STRIDE_S > 0 ? CONFIG_CARMIND_ML_STRIDE_S : 10) * 1000)
static const os_ai_contract_t AI[OS_AI_N] = {
    [OS_AI_EV]  = { "CarMind E.V",  "ml", OS_AI_MODEL, ML_MS, ML_MS, CONFIG_CARMIND_ML_STRIDE_S > 0 ? 10000 : 0, 0, 0,
                    OS_ST_VECTOR, -1, OS_SVC_ML, 3, 1 },
    [OS_AI_REV] = { "CarMind R.E.V", "ml", OS_AI_MODEL, ML_MS, ML_MS, CONFIG_CARMIND_ML_STRIDE_S > 0 ? 10000 : 0, 0, 0,
                    OS_ST_VECTOR, OS_AI_EV, OS_SVC_ML, 3, 1 },
    [OS_AI_REV_LEARNER] = { "R.E.V learner", "rev", OS_AI_LEARNER, ML_MS, 30000, 60000, 20, 30, OS_ST_NONE, -1,
                            OS_SVC_LEARNER, 2, 1 },
};

const os_ai_contract_t *os_ai_contract(int id) { return id >= 0 && id < OS_AI_N ? &AI[id] : NULL; }

int os_ai_check(const os_ai_contract_t *t, int n, char *why, size_t why_n)
{
    int bad = 0;
#define RULE(c, ...) do { if (!(c)) { if (!bad++ && why) snprintf(why, why_n, __VA_ARGS__); } } while (0)
    for (int i = 0; i < n; i++) {
        const os_ai_contract_t *c = &t[i];
        RULE(c->name && c->job && c->period_ms && c->deadline_ms, "%d: name, job, period, deadline", i);
        RULE(!c->stall_ms || c->stall_ms >= c->period_ms, "%s: stall limit shorter than the period", c->name);
        RULE(c->share_pm <= 1000 && (!c->share_pm || c->burst_ms), "%s: reservation needs share <= 1000 and a burst", c->name);
        RULE(c->kind != OS_AI_MODEL || c->selftest != OS_ST_NONE, "%s: a model needs a self-test", c->name);
        RULE(c->svc < OS_SVC_N && c->core >= 0 && c->core <= 1, "%s: runlevel service and core", c->name);
        RULE(c->fallback < n && c->fallback != i, "%s: fallback", c->name);
        if (c->fallback >= 0 && c->fallback < n) RULE(t[c->fallback].kind == OS_AI_MODEL, "%s: falls back to a non-model", c->name);
        int k = i, steps = 0;
        while (k >= 0 && k < n && steps <= n) { k = t[k].fallback; steps++; }
        RULE(steps <= n, "%s: fallback cycle", c->name);
        for (int j = 0; j < i; j++)
            if (c->job && t[j].job && !strcmp(c->job, t[j].job))
                RULE(c->period_ms == t[j].period_ms && c->deadline_ms == t[j].deadline_ms && c->stall_ms == t[j].stall_ms &&
                     c->prio == t[j].prio && c->core == t[j].core, "%s and %s share a job but not its timing", c->name, t[j].name);
    }
#undef RULE
    return bad;
}
