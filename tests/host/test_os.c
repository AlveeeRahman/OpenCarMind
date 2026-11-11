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
}

int main(void)
{
    test_reset_plans();
    test_runlevels();
    test_health();
    printf("os: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
