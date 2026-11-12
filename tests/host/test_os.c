/* Host tests for the mossREV OS layer's pure rules (main/os_core.c). Build and run: make -C tests/host os */
#include <stdio.h>
#include <string.h>
#include "os_core.h"
#include "reset_class.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------- 1. resets ---------- */
static void test_reset_plans(void)
{
    static const reset_kind_t KINDS[] = { RESET_KIND_POWERON, RESET_KIND_SW, RESET_KIND_PANIC, RESET_KIND_INT_WDT,
        RESET_KIND_TASK_WDT, RESET_KIND_WDT, RESET_KIND_USB, RESET_KIND_DEEPSLEEP, RESET_KIND_BROWNOUT, RESET_KIND_OTHER };
    for (int r = 0; r < SYS_RESET_N; r++) {
        const os_reset_plan_t *p = os_reset_plan((sys_reset_reason_t)r);
        CHECK(p && p->name && p->name[0], "reason %d has a plan", r);
        if (!p) continue;
        uint8_t st[5];
        int n = os_reset_steps((sys_reset_reason_t)r, st);
        CHECK(n >= 2 && n <= 5, "reason %d: %d steps", r, n);
        for (int i = 1; i < n; i++) CHECK(st[i] > st[i - 1], "reason %d: steps in the fixed order (%d after %d)", r, st[i], st[i - 1]);
        CHECK(st[n - 1] == OS_STEP_RESET, "reason %d ends with the reset", r);
        int q = 0, m = 0;
        for (int i = 0; i < n; i++) { q |= st[i] == OS_STEP_QUIESCE; m |= st[i] == OS_STEP_MARK; }
        CHECK(q, "reason %d quiesces storage", r);
        CHECK(m == p->asked_for, "reason %d: marker step iff asked for", r);
        /* the next boot's classification (reset_class.c) for every reset kind the chip may report afterwards */
        for (size_t k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++) {
            bool monitor = r == SYS_RESET_MONITOR;
            bool abn = reset_is_abnormal(KINDS[k], monitor, p->asked_for);
            if (p->asked_for) CHECK(!abn, "reason %d, kind %d: an asked-for reset never counts", r, (int)KINDS[k]);
        }
    }
    CHECK(reset_is_abnormal(RESET_KIND_PANIC, false, os_reset_plan(SYS_RESET_UI_STALL)->asked_for), "the stall guard's abort counts");
    CHECK(reset_is_abnormal(RESET_KIND_SW, true, os_reset_plan(SYS_RESET_MONITOR)->asked_for), "the monitor's restart counts");
    CHECK(!reset_is_abnormal(RESET_KIND_PANIC, false, os_reset_plan(SYS_RESET_DOOR)->asked_for), "a door flash (reads as a panic) does not count");
    CHECK(os_reset_plan(SYS_RESET_DOOR)->method == OS_RST_ROM_DOWNLOAD && os_reset_plan(SYS_RESET_DOOR)->post_us == 100000,
          "the door: ROM download after 100 ms");
    CHECK(os_reset_plan(SYS_RESET_UI_STALL)->method == OS_RST_ABORT, "the stall guard aborts (core dump)");
    CHECK(os_reset_plan(SYS_RESET_REBOOT)->method == OS_RST_RESTART && os_reset_plan(SYS_RESET_FORMAT)->method == OS_RST_RESTART,
          "REBOOT and format restart");
    CHECK(os_reset_plan(SYS_RESET_N) == NULL && os_reset_plan((sys_reset_reason_t)-1) == NULL, "unknown reasons have no plan");
    uint8_t st[5];
    CHECK(os_reset_steps(SYS_RESET_N, st) == 0, "unknown reason: no steps");
}

/* ---------- 2. runlevels ----------
   The checks the runlevels replaced, as the code had them before (1.2.0): a drive refused the model switch (door and
   screen), the clock and the format; the car link (sys_mode DRIVE) refused every door command and the Wi-Fi radio; safe
   mode kept the model and the learner off. The new lookup must refuse everything the old checks refused, in every
   combination of inputs. Where it refuses more, that is listed and counted. */
static int old_refused(os_svc_t s, const os_rl_in_t *in)
{
    switch (s) {
    case OS_SVC_MODEL_SWITCH: case OS_SVC_CLOCK_SET: case OS_SVC_FORMAT: return in->drive;
    case OS_SVC_DOOR: case OS_SVC_WIFI_RADIO: return in->link;
    case OS_SVC_ML: case OS_SVC_LEARNER: return in->safe;
    case OS_SVC_WIFI_EDIT: return in->link;   /* the door's add and forget sat behind the door's car-link check */
    default: return 0;
    }
}

static void test_runlevels(void)
{
    int extra[OS_SVC_N] = { 0 }, combos = 0;
    for (int m = 0; m < 64; m++) {
        os_rl_in_t in = { m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1, (m >> 4) & 1, (m >> 5) & 1 };
        uint8_t a = os_rl_active(&in);
        combos++;
        CHECK(a != 0, "some runlevel is always active");
        CHECK(((a >> OS_RL_PARKED) & 1) == (m == 0), "Parked iff nothing else holds (inputs 0x%02x)", m);
        for (int s = 0; s < OS_SVC_N; s++) {
            bool ok = os_rl_allows(a, (os_svc_t)s);
            os_runlevel_t b = os_rl_blocker(a, (os_svc_t)s);
            CHECK(ok == (b == OS_RL_N), "allows and blocker agree (svc %d, inputs 0x%02x)", s, m);
            if (old_refused((os_svc_t)s, &in)) CHECK(!ok, "svc %s: refused as before (inputs 0x%02x)", os_svc_name((os_svc_t)s), m);
            else if (!ok) extra[s]++;
            if (!ok) CHECK(b < OS_RL_N && (a >> b) & 1 && !((os_svc_mask((os_svc_t)s) >> b) & 1), "the blocker is active and refuses");
        }
    }
    /* the intended differences, all from Boot, Saving, Update, Safe for a switch, and "driving" = drive or car link */
    os_rl_in_t parked = { 0 }, boot = { .boot = 1 }, safe = { .safe = 1 }, drive = { .drive = 1 }, link = { .link = 1 },
               upd = { .update = 1 }, sav = { .saving = 1 }, safe_drive = { .safe = 1, .drive = 1 };
    for (int s = 0; s < OS_SVC_N; s++) CHECK(os_rl_allows(os_rl_active(&parked), (os_svc_t)s), "Parked allows %s", os_svc_name((os_svc_t)s));
    CHECK(os_rl_allows(os_rl_active(&boot), OS_SVC_ML) && os_rl_allows(os_rl_active(&boot), OS_SVC_LEARNER),
          "Boot: the model loads and the learner starts");
    CHECK(!os_rl_allows(os_rl_active(&boot), OS_SVC_MODEL_SWITCH) && !os_rl_allows(os_rl_active(&boot), OS_SVC_FORMAT),
          "Boot: no model switch, no format");
    CHECK(os_rl_allows(os_rl_active(&boot), OS_SVC_DOOR) && os_rl_allows(os_rl_active(&boot), OS_SVC_CLOCK_SET), "Boot: door and clock as before");
    CHECK(os_rl_allows(os_rl_active(&safe), OS_SVC_FORMAT) && os_rl_allows(os_rl_active(&safe), OS_SVC_CLOCK_SET) &&
          os_rl_allows(os_rl_active(&safe), OS_SVC_DOOR) && os_rl_allows(os_rl_active(&safe), OS_SVC_WIFI_EDIT),
          "Safe keeps the recovery paths (format, clock, door, Wi-Fi)");
    CHECK(!os_rl_allows(os_rl_active(&safe), OS_SVC_MODEL_SWITCH), "Safe: no model switch (it would load a model)");
    CHECK(!os_rl_allows(os_rl_active(&safe_drive), OS_SVC_LEARNER) && !os_rl_allows(os_rl_active(&safe_drive), OS_SVC_CLOCK_SET),
          "safe mode while driving: both breakers apply");
    CHECK(os_rl_allows(os_rl_active(&drive), OS_SVC_ML) && os_rl_allows(os_rl_active(&drive), OS_SVC_LEARNER), "Driving runs the model and the learner");
    CHECK(!os_rl_allows(os_rl_active(&link), OS_SVC_MODEL_SWITCH), "the car link counts as driving for a switch (new)");
    for (int s = 0; s < OS_SVC_N; s++) {
        if (s == OS_SVC_ML) continue;
        CHECK(!os_rl_allows(os_rl_active(&upd), (os_svc_t)s), "Update refuses %s", os_svc_name((os_svc_t)s));
        if (s != OS_SVC_ML) CHECK(!os_rl_allows(os_rl_active(&sav), (os_svc_t)s) || s == OS_SVC_ML, "Saving refuses %s", os_svc_name((os_svc_t)s));
    }
    CHECK(os_rl_primary(os_rl_active(&safe_drive)) == OS_RL_DRIVING, "shown: Driving over Safe");
    os_rl_in_t all = { 1, 1, 1, 1, 1, 1 };
    CHECK(os_rl_primary(os_rl_active(&all)) == OS_RL_UPDATE, "shown: Update first");
    CHECK(os_rl_blocker(os_rl_active(&all), OS_SVC_FORMAT) == OS_RL_UPDATE, "the blocker is the highest refusing runlevel");
    CHECK(!strcmp(os_rl_phrase(OS_RL_DRIVING, &drive), "a drive is running"), "drive phrase (the door's old MODEL SET text)");
    CHECK(!strcmp(os_rl_phrase(OS_RL_DRIVING, &link), "the car link is up"), "link phrase (the door's old busy text)");
    CHECK(!strcmp(os_rl_name(os_rl_primary(os_rl_active(&parked))), "Parked"), "name");
    CHECK(os_svc_mask(OS_SVC_N) == 0 && !os_rl_allows(os_rl_active(&parked), OS_SVC_N), "unknown service never allowed");
    printf("  runlevels: %d input combinations x %d services; refused where the old checks did not:", combos, OS_SVC_N);
    for (int s = 0; s < OS_SVC_N; s++) printf(" %s %d", os_svc_name((os_svc_t)s), extra[s]);
    printf("\n");
}

/* ---------- 3. boot health ---------- */
static void test_health(void)
{
    const os_health_in_t good = { .selftest = 1, .model_expected = true, .nor = 1, .nand = 1, .first_tick_ms = 1900,
                                  .touch_ok = true, .schedulable = true };
    uint16_t f = 0xFFFF;
    char t[200];
    CHECK(os_health_verdict(&good, &f) == OS_H_GOOD && f == 0, "all fine: good");
    os_health_text(OS_H_GOOD, 0, t, sizeof t);
    CHECK(!strcmp(t, "good"), "good text '%s'", t);
    struct { const char *what; os_health_in_t in; uint8_t v; uint16_t flag; } C[] = {
        { "self-test differs",      good, OS_H_DEGRADED, OS_HF_MODEL_DIFF },
        { "self-test not run",      good, OS_H_DEGRADED, OS_HF_MODEL_NOT_RUN },
        { "FAT repaired",           good, OS_H_DEGRADED, OS_HF_NOR_REPAIRED },
        { "audit warning",          good, OS_H_DEGRADED, OS_HF_NOR_WARN },
        { "flash FAT damaged",      good, OS_H_DEGRADED, OS_HF_NOR_DOWN },
        { "flash FAT not mounted",  good, OS_H_DEGRADED, OS_HF_NOR_DOWN },
        { "NAND absent",            good, OS_H_DEGRADED, OS_HF_NAND },
        { "NAND unreadable",        good, OS_H_DEGRADED, OS_HF_NAND },
        { "no UI tick",             good, OS_H_BAD,      OS_HF_NO_FRAME },
        { "slow first tick",        good, OS_H_DEGRADED, OS_HF_SLOW_FRAME },
        { "touch failing",          good, OS_H_DEGRADED, OS_HF_TOUCH },
        { "misses",                 good, OS_H_DEGRADED, OS_HF_MISSES },
        { "stalls",                 good, OS_H_DEGRADED, OS_HF_STALLS },
        { "not schedulable",        good, OS_H_DEGRADED, OS_HF_UNSCHED },
        { "safe mode",              good, OS_H_BAD,      OS_HF_SAFE },
    };
    C[0].in.selftest = 0; C[1].in.selftest = -1; C[2].in.nor_repairs = 2; C[3].in.nor_warn = true; C[4].in.nor = -1;
    C[5].in.nor = 0; C[6].in.nand = 0; C[7].in.nand = -1; C[8].in.first_tick_ms = 0; C[9].in.first_tick_ms = OS_H_SLOW_FRAME_MS + 1;
    C[10].in.touch_ok = false; C[11].in.misses = 3; C[12].in.stalls = 1; C[13].in.schedulable = false; C[14].in.safe = true;
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        uint8_t v = os_health_verdict(&C[i].in, &f);
        CHECK(v == C[i].v && f == C[i].flag, "%s: verdict %d flags 0x%x", C[i].what, v, f);
    }
    os_health_in_t x = good;
    x.model_expected = false; x.selftest = -1;
    CHECK(os_health_verdict(&x, &f) == OS_H_GOOD, "no model expected (safe mode or none built): its self-test is not asked");
    x = good; x.nand = 2;
    CHECK(os_health_verdict(&x, &f) == OS_H_GOOD, "NAND not built: not a fault");
    x = good; x.first_tick_ms = OS_H_SLOW_FRAME_MS;
    CHECK(os_health_verdict(&x, &f) == OS_H_GOOD, "first tick at the limit: good");
    x = C[0].in; x.nand = 0; x.safe = true;
    CHECK(os_health_verdict(&x, &f) == OS_H_BAD, "Bad wins over Degraded");
    os_health_text(OS_H_DEGRADED, OS_HF_MODEL_DIFF | OS_HF_NAND, t, sizeof t);
    CHECK(!strcmp(t, "degraded: model self-test differs, NAND not mounted"), "text '%s'", t);
    os_health_text(OS_H_BAD, 0x1FFF, t, 24);
    CHECK(strlen(t) == 23, "text truncates to the buffer");
    CHECK(!strcmp(os_health_name(OS_H_PENDING), "pending") && !strcmp(os_health_name(9), "?"), "names");
    /* A/B rollback (prepared, CONFIG_OCM_APP_ROLLBACK): only a pending image is judged, Bad rolls back */
    CHECK(os_rollback_decision(true, OS_H_GOOD) == OS_RB_CONFIRM && os_rollback_decision(true, OS_H_DEGRADED) == OS_RB_CONFIRM,
          "pending + good or degraded: confirm");
    CHECK(os_rollback_decision(true, OS_H_BAD) == OS_RB_ROLLBACK, "pending + bad: roll back");
    CHECK(os_rollback_decision(true, OS_H_PENDING) == OS_RB_NOTHING && os_rollback_decision(false, OS_H_BAD) == OS_RB_NOTHING,
          "no verdict yet, or an image that is not pending: nothing");
    const os_reset_plan_t *rb = os_reset_plan(SYS_RESET_ROLLBACK);
    CHECK(rb && rb->asked_for && rb->quiesce && rb->method == OS_RST_ROLLBACK,
          "the rollback quiesces and is asked for (the old image must not inherit the crash count)");
}

/* ---------- 3b. the Wi-Fi boot hold ---------- */
static void test_wifi_hold(void)
{
    CHECK(os_wifi_may_start(OS_H_GOOD, false), "verdict good: the automatic Wi-Fi start goes ahead");
    CHECK(os_wifi_may_start(OS_H_DEGRADED, false), "verdict degraded: the image runs, Wi-Fi starts (as the rollback confirms it)");
    CHECK(!os_wifi_may_start(OS_H_BAD, false), "verdict bad: the automatic start is refused");
    CHECK(!os_wifi_may_start(OS_H_PENDING, false), "no verdict yet (the boot window): refused");
    CHECK(!os_wifi_may_start(9, false), "an unknown verdict: refused");
    for (int v = 0; v <= 9; v++) CHECK(os_wifi_may_start((uint8_t)v, true), "the owner's start from the screen works with verdict %d", v);
    /* from the real inputs: every Bad cause holds Wi-Fi, every Degraded cause and Good let it start; the owner always can */
    const os_health_in_t good = { .selftest = 1, .model_expected = true, .nor = 1, .nand = 1, .first_tick_ms = 1900,
                                  .touch_ok = true, .schedulable = true };
    unsigned seed = 12345, held = 0, started = 0;
    for (int i = 0; i < 20000; i++) {
        os_health_in_t in = good;
        unsigned r = seed = seed * 1103515245u + 12345u;
        if (r & 1) in.selftest = (int8_t)((r >> 1) % 3) - 1;
        if (r & 8) in.nor = (int8_t)((r >> 4) % 3) - 1;
        if (r & 64) in.nor_repairs = (uint16_t)((r >> 7) & 3);
        if (r & 512) in.nor_warn = true;
        if (r & 1024) in.nand = (int8_t)((r >> 11) % 4) - 1;
        if ((r >> 13) % 7 == 0) in.first_tick_ms = 0;
        else if ((r >> 13) % 7 == 1) in.first_tick_ms = OS_H_SLOW_FRAME_MS + 1;
        if ((r >> 16) % 11 == 0) in.touch_ok = false;
        if ((r >> 17) % 5 == 0) in.misses = (r >> 20) & 7;
        if ((r >> 18) % 9 == 0) in.stalls = 1;
        if ((r >> 19) % 13 == 0) in.schedulable = false;
        if ((r >> 21) % 6 == 0) in.safe = true;
        uint16_t f;
        uint8_t v = os_health_verdict(&in, &f);
        bool bad = (in.first_tick_ms == 0) || in.safe;
        CHECK(os_wifi_may_start(v, false) == !bad, "input %d: verdict %d, held %d", i, v, bad);
        CHECK(os_wifi_may_start(v, true), "input %d: the owner can start Wi-Fi", i);
        if (bad) held++; else started++;
    }
    CHECK(held > 1000 && started > 1000, "both outcomes covered (%u held, %u started)", held, started);
    printf("  wifi hold: 20000 random health inputs, %u held (bad), %u started (good or degraded), owner start always allowed\n",
           held, started);
}

/* ---------- 4. response-time analysis ---------- */
/* sched_mon.c's analyse() loop, copied as it is in 1.2.0, as a reference (it stops after 50 steps) */
static uint32_t sched_mon_wcrt(const os_rt_task_t *t, int n, int i)
{
    uint64_t r = t[i].c_us, prev = 0, limit = t[i].d_us;
    for (int it = 0; it < 50 && r != prev && r <= limit; it++) {
        prev = r;
        r = t[i].c_us;
        for (int j = 0; j < n; j++) {
            if (j == i || t[j].core != t[i].core || t[j].prio <= t[i].prio) continue;
            uint64_t tj = t[j].t_us;
            r += ((prev + tj - 1) / tj) * t[j].c_us;
        }
    }
    return r > UINT32_MAX ? UINT32_MAX : (uint32_t)r;
}

static uint32_t rng32(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

static void test_rta(void)
{
    /* the classic three-task example: C 3/3/5, T 7/12/20, D = T, rate-monotonic priorities. By hand:
       R1 = 3; R2 = 3 + ceil(3/7)3 = 6, then 3 + ceil(6/7)3 = 6; R3: 5 -> 11 -> 14 -> 17 -> 20 -> 20 */
    os_rt_task_t t[3] = { { "a", 3, 7, 7, 3, 1 }, { "b", 3, 12, 12, 2, 1 }, { "c", 5, 20, 20, 1, 1 } };
    uint32_t r[OS_RTA_MAX];
    CHECK(os_rta(t, 3, 1, r) == 0 && r[0] == 3 && r[1] == 6 && r[2] == 20, "classic example: R = %u %u %u", r[0], r[1], r[2]);
    t[2].c_us = 6;
    CHECK(os_rta(t, 3, 1, r) == 1 && r[2] == OS_RTA_MISS && r[0] == 3 && r[1] == 6, "C3 = 6: the lowest task misses");
    t[2].c_us = 5; t[2].d_us = 19;
    CHECK(os_rta(t, 3, 1, r) == 1, "D3 = 19 < R3 = 20: a miss");
    t[2].d_us = 20; t[1].core = 0;
    CHECK(os_rta(t, 3, 1, r) == 0 && r[1] == 0 && r[2] == 5 + 2 * 3, "a task on the other core does not interfere (R3 %u)", r[2]);
    os_rt_task_t eq[2] = { { "x", 4, 10, 10, 2, 1 }, { "y", 4, 10, 10, 2, 1 } };
    CHECK(os_rta(eq, 2, 1, r) == 0 && r[0] == 4 && r[1] == 4, "equal priorities do not interfere (as sched_mon)");
    os_rt_task_t over[2] = { { "hi", 10, 10, 10, 3, 1 }, { "lo", 5, 1000, 1000, 1, 1 } };
    CHECK(os_rta(over, 2, 1, r) == 1 && r[1] == OS_RTA_MISS, "U = 1 on the core: the low task never fits inside D");
    /* the same answers as sched_mon's own loop on random task sets where that loop converged */
    uint32_t seed = 12345, agree = 0, cases = 0;
    for (int c = 0; c < 20000; c++) {
        int n = 2 + (int)(rng32(&seed) % 6);
        os_rt_task_t s[8];
        for (int i = 0; i < n; i++) {
            uint32_t T = 1000 + rng32(&seed) % 200000;
            s[i] = (os_rt_task_t){ "t", 1 + rng32(&seed) % (T / 4), T, T, (uint8_t)(1 + rng32(&seed) % 5), (int8_t)(rng32(&seed) % 2) };
        }
        uint32_t ra[8], rb[8];
        os_rta(s, n, 0, ra);
        os_rta(s, n, 1, rb);
        for (int i = 0; i < n; i++) {
            uint32_t mine = s[i].core ? rb[i] : ra[i], ref = sched_mon_wcrt(s, n, i);
            if (ref > s[i].d_us) { cases++; agree += mine == OS_RTA_MISS; continue; }
            cases++;
            agree += mine == ref;
        }
    }
    CHECK(agree == cases, "agrees with sched_mon's loop: %u of %u", agree, cases);
    printf("  rta: classic example R = 3, 6, 20; random sets: %u of %u task results equal to sched_mon's\n", agree, cases);

    /* admission: core 1 as on the board (obd prio 5, ml 3, rev 2), measured C from sched_mon */
    os_rt_task_t b[3] = { { "obd", 30000, 200000, 200000, 5, 1 }, { "rev", 20000, 1000000, 1000000, 2, 1 },
                          { "ui", 40000, 50000, 50000, 2, 0 } };
    const os_rt_task_t ml = { "ml", 0, 1000000, 1000000, 3, 1 };
    os_admit_t a;
    CHECK(os_admit(b, 3, &ml, 2000, OS_ADMIT_FACTOR_PM, &a) && a.c_us == 4000, "R.E.V-size run admitted, C x2 = %u", a.c_us);
    CHECK(a.r_us[3] == 4000 + 30000 && a.r_us[1] == 20000 + 4000 + 30000 && a.r_us[2] == 0, "R ml %u, rev %u, ui not analysed",
          a.r_us[3], a.r_us[1]);
    CHECK(a.margin_us == 200000 - 30000 && a.worst == 0, "margin %d us at %d (obd: R 30 ms of 200)", a.margin_us, a.worst);
    CHECK(!os_admit(b, 3, &ml, 500000, OS_ADMIT_FACTOR_PM, &a) && a.r_us[3] == OS_RTA_MISS && a.margin_us < 0,
          "a 500 ms model x2 = 1 s plus obd misses: refused (margin %d)", a.margin_us);
    CHECK(os_admit(b, 3, &ml, 450000, 1000, &a), "the same set admits 450 ms with no factor");
    CHECK(!os_admit(b, 3, &ml, 450000, OS_ADMIT_FACTOR_PM, &a), "and refuses it with the factor 2");
    b[1].d_us = 60000;
    CHECK(!os_admit(b, 3, &ml, 6000, OS_ADMIT_FACTOR_PM, &a) && a.worst == 1, "a lower-priority job's deadline counts too (rev)");
    b[1].d_us = 1000000; b[1].core = -1;
    CHECK(os_admit(b, 3, &ml, 6000, OS_ADMIT_FACTOR_PM, &a) && a.r_us[1] == 0, "a job that never ran is not analysed");
}

/* ---------- 5. AI service contract ---------- */
static void test_contract(void)
{
    os_ai_contract_t t[OS_AI_N];
    char why[96] = "";
    for (int i = 0; i < OS_AI_N; i++) t[i] = *os_ai_contract(i);
    CHECK(os_ai_check(t, OS_AI_N, why, sizeof why) == 0, "the shipped table keeps its rules (%s)", why);
    const os_ai_contract_t *ev = os_ai_contract(OS_AI_EV), *rev = os_ai_contract(OS_AI_REV), *ln = os_ai_contract(OS_AI_REV_LEARNER);
    /* the values the code registered before 2.0.0 (ml_infer.cpp, rev_task.c, stride 1) */
    CHECK(ev->period_ms == 1000 && ev->deadline_ms == 1000 && ev->stall_ms == 10000 && ev->prio == 3 && ev->core == 1 && !ev->share_pm,
          "ml job: T 1 s, D 1 s, stall 10 s, prio 3, core 1, no reservation");
    CHECK(ln->period_ms == 1000 && ln->deadline_ms == 30000 && ln->stall_ms == 60000 && ln->share_pm == 20 && ln->burst_ms == 30 &&
          ln->prio == 2 && ln->core == 1, "rev job: T 1 s, D 30 s, stall 60 s, 2 %% in 30 ms bursts, prio 2, core 1");
    CHECK(rev->fallback == OS_AI_EV && ev->fallback == -1 && ln->fallback == -1, "R.E.V falls back to E.V (ml_init's old rule)");
    CHECK(ev->svc == OS_SVC_ML && ln->svc == OS_SVC_LEARNER, "runlevel services");
    CHECK(os_ai_contract(OS_AI_N) == NULL && os_ai_contract(-1) == NULL, "unknown ids");
    struct { const char *what; void (*mut)(os_ai_contract_t *); } M[] = {
        { "stall < period",       NULL }, { "share without burst", NULL }, { "share > 1000", NULL }, { "model w/o self-test", NULL },
        { "fallback to itself",   NULL }, { "fallback cycle",      NULL }, { "fallback to learner", NULL },
        { "shared job, other D",  NULL }, { "bad core",            NULL },
    };
    for (int m = 0; m < (int)(sizeof M / sizeof M[0]); m++) {
        for (int i = 0; i < OS_AI_N; i++) t[i] = *os_ai_contract(i);
        switch (m) {
        case 0: t[OS_AI_EV].stall_ms = 500; break;
        case 1: t[OS_AI_REV_LEARNER].burst_ms = 0; break;
        case 2: t[OS_AI_REV_LEARNER].share_pm = 1001; break;
        case 3: t[OS_AI_REV].selftest = OS_ST_NONE; break;
        case 4: t[OS_AI_EV].fallback = OS_AI_EV; break;
        case 5: t[OS_AI_EV].fallback = OS_AI_REV; break;
        case 6: t[OS_AI_REV].fallback = OS_AI_REV_LEARNER; break;
        case 7: t[OS_AI_REV].deadline_ms = 900; break;
        case 8: t[OS_AI_REV_LEARNER].core = 2; break;
        }
        CHECK(os_ai_check(t, OS_AI_N, why, sizeof why) > 0, "mutation caught: %s", M[m].what);
    }
}

/* ---------- 6. UPS power state ---------- */
static void test_ups(void)
{
    const os_ups_cfg_t c = { 20, 15000, 3400 };
    os_ups_t u = { 0 };
    uint32_t t = 0;
    CHECK(os_ups_step(&u, &c, true, 40, 4100, t) == 0 && u.state == OS_UPS_USB, "charging: on USB");
    CHECK(os_ups_step(&u, &c, true, -3, 4150, t += 2000) == 0 && u.state == OS_UPS_USB, "full, about 0 mA: on USB");
    CHECK(os_ups_step(&u, &c, true, -120, 4000, t += 2000) == 0 && u.state == OS_UPS_USB, "one discharge sample is not enough");
    CHECK(os_ups_step(&u, &c, true, 10, 4100, t += 2000) == 0 && u.low_samples == 0, "and a USB sample clears it");
    os_ups_step(&u, &c, true, -120, 4000, t += 2000);
    CHECK(os_ups_step(&u, &c, true, -120, 3990, t += 2000) == OS_UPS_A_FLUSH_DIM && u.state == OS_UPS_GRACE, "two in a row: flush and dim");
    /* cranking: USB comes back inside the grace */
    CHECK(os_ups_step(&u, &c, true, -300, 3800, t += 2000) == 0 && u.state == OS_UPS_GRACE, "cranking dip, still waiting");
    CHECK(os_ups_step(&u, &c, true, 30, 4000, t += 2000) == OS_UPS_A_RESTORE && u.state == OS_UPS_USB, "USB back: restore");
    /* a real loss: grace runs out */
    os_ups_step(&u, &c, true, -120, 4000, t += 2000);
    os_ups_step(&u, &c, true, -120, 4000, t += 2000);
    uint32_t t_loss = t;
    int off_at = -1;
    for (int i = 1; i <= 10; i++) {
        uint8_t a = os_ups_step(&u, &c, true, -120, 3950, t += 2000);
        if (a == OS_UPS_A_POWER_OFF) { off_at = (int)(t - t_loss); break; }
    }
    CHECK(off_at >= 15000 && off_at < 17000 && u.state == OS_UPS_OFF, "power off after the 15 s grace (%d ms)", off_at);
    CHECK(os_ups_step(&u, &c, true, 50, 4100, t += 2000) == 0 && u.state == OS_UPS_OFF, "OFF is final");
    /* the floor: at once from USB-side states, on battery only */
    os_ups_t v = { 0 };
    CHECK(os_ups_step(&v, &c, true, -120, 3400, 0) == OS_UPS_A_POWER_OFF, "at the floor on battery: off at once");
    os_ups_t w = { 0 };
    CHECK(os_ups_step(&w, &c, true, 40, 3300, 0) == 0 && w.state == OS_UPS_USB, "a low cell while charging is not a power loss");
    CHECK(os_ups_step(&w, &c, false, -500, 3000, 2000) == 0 && w.state == OS_UPS_USB, "no gauge reading: nothing changes");
    os_ups_t x = { 0 };
    for (int i = 0; i < 30; i++) os_ups_step(&x, &c, true, -19, 4000, (uint32_t)i * 2000);
    CHECK(x.state == OS_UPS_USB, "-19 mA (above the -20 mA threshold) never counts as battery");
    CHECK(!strcmp(os_ups_name(OS_UPS_GRACE), "battery grace"), "name");
    const os_reset_plan_t *p = os_reset_plan(SYS_RESET_POWER_OFF);
    CHECK(p && p->method == OS_RST_POWER_OFF && p->quiesce && p->asked_for, "power off: quiesce first, not a crash");
    os_rl_in_t sv = { .saving = 1 };
    CHECK(!os_rl_allows(os_rl_active(&sv), OS_SVC_FORMAT) && !os_rl_allows(os_rl_active(&sv), OS_SVC_MODEL_SWITCH), "grace is the Saving runlevel");
}

/* ---------- 7. degradation ladder ---------- */
static void test_ladder(void)
{
    os_ladder_t l = { 0 };
    uint32_t m = 7;                                       /* misses from before the ladder ran do not count */
    CHECK(os_ladder_step(&l, m, false) == OS_DL_NORMAL, "priming window");
    CHECK(os_ladder_step(&l, m, false) == OS_DL_NORMAL, "no new miss: normal");
    CHECK(os_ladder_step(&l, ++m, true) == OS_DL_UI_SLOW, "first miss: UI slowed first");
    CHECK(os_ladder_step(&l, m += 3, true) == OS_DL_NO_LEARNER, "more misses: the learner's reservation goes next");
    CHECK(os_ladder_step(&l, ++m, true) == OS_DL_MODEL_OFF, "still missing: the model pauses last");
    CHECK(os_ladder_step(&l, ++m, true) == OS_DL_MODEL_OFF, "the top rung holds");
    int lvl = 0;
    for (int i = 0; i < 30; i++) lvl = os_ladder_step(&l, m, true);
    CHECK(lvl == OS_DL_MODEL_OFF, "while driving the model stays paused (no on/off within one drive)");
    CHECK(os_ladder_step(&l, m, false) == OS_DL_NO_LEARNER, "parked after a clean minute of driving: one rung down at once");
    os_ladder_t p2 = { .level = OS_DL_MODEL_OFF, .primed = true, .last_misses = 5 };
    for (int i = 0; i < OS_DL_CLEAN - 1; i++) lvl = os_ladder_step(&p2, 5, false);
    CHECK(lvl == OS_DL_MODEL_OFF, "parked, 5 clean windows: still paused");
    CHECK(os_ladder_step(&p2, 5, false) == OS_DL_NO_LEARNER, "parked, the sixth clean window: one rung down");
    for (int i = 0; i < OS_DL_CLEAN; i++) lvl = os_ladder_step(&l, m, true);
    CHECK(lvl == OS_DL_UI_SLOW, "lower rungs step down while driving too");
    CHECK(os_ladder_step(&l, ++m, true) == OS_DL_NO_LEARNER, "a miss climbs again and clears the clean count");
    for (int i = 0; i < OS_DL_CLEAN - 1; i++) lvl = os_ladder_step(&l, m, true);
    CHECK(lvl == OS_DL_NO_LEARNER, "the clean count restarted at the miss");
    for (int i = 0; i < 3 * OS_DL_CLEAN; i++) lvl = os_ladder_step(&l, m, true);
    CHECK(lvl == OS_DL_NORMAL, "back to normal after enough clean windows");
    os_ladder_t w = { 0 };
    os_ladder_step(&w, 0xFFFFFFFFu, false);
    CHECK(os_ladder_step(&w, 0, false) == OS_DL_UI_SLOW, "counter wrap counts as a new miss, never as none");
    CHECK(!strcmp(os_ladder_name(OS_DL_MODEL_OFF), "model paused"), "name");
}

/* ---------- 8. battery safety ---------- */
static void test_battery(void)
{
    const os_bat_cfg_t c = { 450, 30, 600, 4250, 2, 15 };
    os_bat_t b = { 0 };
    uint8_t a;
    CHECK(os_bat_step(&b, &c, true, 300, 4100, true) == 0 && b.state == OS_BAT_OK, "30 C on USB: ok");
    CHECK(os_bat_step(&b, &c, true, 455, 4100, true) == 0 && b.state == OS_BAT_OK, "one hot sample is not enough");
    CHECK(os_bat_step(&b, &c, true, 456, 4100, true) == OS_BAT_A_SHED && b.state == OS_BAT_HOT, "two at 45 C on USB: shed");
    CHECK(os_bat_step(&b, &c, true, 430, 4100, true) == 0 && b.state == OS_BAT_HOT, "43 C: inside the hysteresis, still hot");
    os_bat_step(&b, &c, true, 420, 4100, true);
    CHECK(os_bat_step(&b, &c, true, 419, 4100, true) == OS_BAT_A_UNSHED && b.state == OS_BAT_OK, "two at or below 42 C: back to ok");
    os_bat_t nb = { 0 };
    os_bat_step(&nb, &c, true, 500, 3900, false);
    CHECK(os_bat_step(&nb, &c, true, 500, 3900, false) == 0 && nb.state == OS_BAT_OK, "50 C on battery: no shed (the rule is for USB)");
    os_bat_t g = { 0 };
    os_bat_step(&g, &c, true, 456, 4100, true);
    CHECK(os_bat_step(&g, &c, true, 3000, 4100, true) == 0 && g.n_hot == 1, "a 300 C glitch neither counts nor resets");
    CHECK(os_bat_step(&g, &c, false, 456, 4100, true) == 0 && os_bat_step(&g, &c, true, 456, 0, true) == 0 && g.n_hot == 1,
          "no reading or 0 mV: ignored");
    CHECK(os_bat_step(&g, &c, true, 456, 4100, true) == OS_BAT_A_SHED, "the next real sample confirms");
    /* critical: from hot or ok, on USB or battery; resume only after the cool-down */
    os_bat_step(&g, &c, true, 600, 4100, false);
    CHECK((a = os_bat_step(&g, &c, true, 610, 4100, false)) == OS_BAT_A_STOP && g.state == OS_BAT_CRIT, "two at 60 C: stop");
    int resume_at = -1;
    for (int i = 0; i < 40; i++) {
        a = os_bat_step(&g, &c, true, i < 5 ? 580 : 560, 4100, false);   /* 58 C is inside the hysteresis */
        if (a == OS_BAT_A_RESUME) { resume_at = i; break; }
    }
    CHECK(resume_at == 5 + 14 && g.state == OS_BAT_OK, "resume after 15 samples at or below 57 C (at %d)", resume_at);
    /* over-voltage: final until a restart */
    os_bat_t v = { 0 };
    os_bat_step(&v, &c, true, 300, 4250, true);
    CHECK(os_bat_step(&v, &c, true, 300, 4260, true) == OS_BAT_A_STOP && v.state == OS_BAT_OVERVOLT, "two at 4.25 V: stop");
    for (int i = 0; i < 50; i++) a = os_bat_step(&v, &c, true, 250, 3900, true);
    CHECK(a == 0 && v.state == OS_BAT_OVERVOLT, "over-voltage never resumes by itself");
    os_bat_t w = { 0 };
    os_bat_step(&w, &c, true, 300, 4249, true);
    CHECK(os_bat_step(&w, &c, true, 300, 4249, true) == 0 && w.state == OS_BAT_OK, "4.249 V: below the fault");
    CHECK(!strcmp(os_bat_name(OS_BAT_HOT), "hot: load shed"), "name");
    /* the shed is the Saving runlevel: Wi-Fi radio and the learner stop, the model runs */
    os_rl_in_t sv = { .saving = 1 };
    CHECK(!os_rl_allows(os_rl_active(&sv), OS_SVC_WIFI_RADIO) && !os_rl_allows(os_rl_active(&sv), OS_SVC_LEARNER) &&
          os_rl_allows(os_rl_active(&sv), OS_SVC_ML), "Saving: no Wi-Fi radio, no learner, the model stays");
    /* 9. the BQ27220 helpers */
    uint8_t d[2] = { 0x00, 0x41 };                          /* 65 mAh, big endian */
    CHECK(os_bq_checksum(0x929F, d, 2) == (uint8_t)(0xFF - (0x9F + 0x92 + 0x00 + 0x41)), "block checksum");
    CHECK(os_bq_design_energy(65, 3700) == 241 && os_bq_design_energy(3000, 3700) == 11100, "design energy mAh x 3.7 V");
    CHECK(os_bq_cal_ok(3000, 3000, 11100, 65), "factory values pass the pre-write check");
    CHECK(!os_bq_cal_ok(2999, 3000, 11100, 65) && !os_bq_cal_ok(3000, 3000, 2000, 65) && !os_bq_cal_ok(3000, 3000, 13000, 65),
          "a wrong address (other capacity or energy) is refused");
    CHECK(!os_bq_cal_ok(3000, 3000, 11100, 10) && !os_bq_cal_ok(3000, 3000, 11100, 6000) && !os_bq_cal_ok(0, 0, 0, 65), "target range");
}

/* the car-link input (owner 2026-11-11): Driving only once the car's computer answers. Every Bluetooth state, alone and
   with each other input, through the unchanged masks. */
static void test_link_input(void)
{
    static const char *const NAME[] = {"idle", "scanning", "connecting", "discovering", "elm init", "logging", "locked"};
    for (int b = BLE_ST_IDLE; b <= BLE_ST_LOCKED; b++) {
        bool link = os_rl_link_input((ble_state_t)b);
        CHECK(link == (b == BLE_ST_LOGGING), "%s: car link %s", NAME[b], link ? "up" : "down");
        os_rl_in_t in = { .link = link };
        os_runlevel_t r = os_rl_primary(os_rl_active(&in));
        CHECK(r == (b == BLE_ST_LOGGING ? OS_RL_DRIVING : OS_RL_PARKED), "%s alone: runlevel %s", NAME[b], os_rl_name(r));
        if (b != BLE_ST_LOGGING) {
            /* an attempt to link (a neighbour's adapter, an adapter in a parked car) refuses nothing Parked allows */
            for (int s = 0; s < OS_SVC_N; s++)
                CHECK(os_rl_allows(os_rl_active(&in), (os_svc_t)s), "%s: %s allowed", NAME[b], os_svc_name((os_svc_t)s));
            CHECK(os_rl_blocker(os_rl_active(&in), OS_SVC_MODEL_SWITCH) == OS_RL_N, "%s: nothing blocks a model switch", NAME[b]);
        } else {
            CHECK(!os_rl_allows(os_rl_active(&in), OS_SVC_MODEL_SWITCH) && !os_rl_allows(os_rl_active(&in), OS_SVC_FORMAT) &&
                  !os_rl_allows(os_rl_active(&in), OS_SVC_CLOCK_SET), "logging: switch, format and clock refused");
            CHECK(!strcmp(os_rl_phrase(OS_RL_DRIVING, &in), "the car link is up"), "logging without a drive: the link phrase");
        }
        /* the other inputs still act on their own */
        os_rl_in_t d = { .link = link, .drive = true };
        CHECK(os_rl_primary(os_rl_active(&d)) == OS_RL_DRIVING, "%s + a recorded drive: Driving", NAME[b]);
        os_rl_in_t u = { .link = link, .update = true };
        CHECK(os_rl_primary(os_rl_active(&u)) == OS_RL_UPDATE, "%s + update: Update shown", NAME[b]);
    }
}

int main(void)
{
    test_reset_plans();
    test_runlevels();
    test_link_input();
    test_health();
    test_wifi_hold();
    test_rta();
    test_contract();
    test_ups();
    test_ladder();
    test_battery();
    printf("os: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
