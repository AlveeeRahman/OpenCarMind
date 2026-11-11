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

int main(void)
{
    test_reset_plans();
    printf("os: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
