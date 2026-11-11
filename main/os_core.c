/* os_core.c: the pure rules of the mossREV OS layer. See os_core.h. */
#include "os_core.h"
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
