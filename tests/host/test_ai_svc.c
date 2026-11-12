/* Host tests for EdgeOS's AI service layer (main/ai_svc_core.c, main/ai_snap.h): a deterministic simulation of the
 * release table under overload (no pile-up, skip accounting, deadline-monotonic order, slow and pause), the degrade
 * order against the real OS ladder (main/os_core.c), and the seqlock and ring. Build and run: make -C tests/host ai */
#include <stdio.h>
#include <string.h>
#include "ai_svc_core.h"
#include "ai_snap.h"
#include "os_core.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ai_rt_t svc(const char *name, uint32_t t_ms, uint32_t d_ms, uint32_t c_us)
{
    ai_rt_t s;
    memset(&s, 0, sizeof s);
    s.name = name; s.period_ms = t_ms; s.deadline_ms = d_ms; s.budget_us = c_us; s.active = true;
    return s;
}

/* A worker that runs step_us(service, job) for every picked job, preempted by nothing (the worst case for the
   services themselves is in the response time). Time advances to the next release when nothing is due. Checks after
   every job that no service ever has more than one job outstanding. */
typedef uint32_t (*step_fn)(int svc, uint32_t job);
static void simulate(ai_rt_t *t, int n, int64_t until_us, uint8_t level, step_fn step, int64_t *now_io, int *max_outstanding)
{
    int64_t now = *now_io;
    while (now < until_us) {
        int64_t wake;
        int i = ai_rt_pick(t, n, now, level, &wake);
        if (i < 0) { now = wake > now ? wake : now + 1000; continue; }
        int out = (int)(t[i].released - t[i].done);
        if (out > *max_outstanding) *max_outstanding = out;
        int64_t start = now;
        now += step(i, t[i].released);
        ai_rt_finish(&t[i], start, now, level);
    }
    *now_io = now;
}

static uint32_t step_ok(int s, uint32_t j) { (void)s; (void)j; return 300; }
static uint32_t step_overrun(int s, uint32_t j) { (void)s; (void)j; return 5000; }          /* budget 2 ms */
static uint32_t step_huge(int s, uint32_t j) { (void)s; (void)j; return 3500000; }        /* 3.5 periods */
static uint32_t step_spike(int s, uint32_t j) { (void)s; return j % 10 == 0 ? 1500000 : 300; }

static void test_release_table(void)
{
    int mo;
    int64_t now;
    /* nominal: one release per second, every job done, nothing missed or skipped */
    ai_rt_t a = svc("bat", 1000, 1000, 2000);
    ai_rt_start(&a, 0);
    now = 0; mo = 0;
    simulate(&a, 1, 100000000, AI_DL_NORMAL, step_ok, &now, &mo);
    CHECK(a.released == 100 && a.done == 100 && a.missed == 0 && a.skipped == 0, "nominal: %u released %u done %u missed %u skipped",
          a.released, a.done, a.missed, a.skipped);
    CHECK(a.wcrt_us == 300 && mo == 1, "nominal: WCRT %u us, outstanding %d", a.wcrt_us, mo);

    /* budget overrun on every job: the job finishes, the next release is skipped: half the releases run */
    ai_rt_t b = svc("bat", 1000, 1000, 2000);
    ai_rt_start(&b, 0);
    now = 0; mo = 0;
    simulate(&b, 1, 100000000, AI_DL_NORMAL, step_overrun, &now, &mo);
    CHECK(b.done == 50 && b.overruns == 50 && b.skipped == 50 && mo == 1, "overrun: %u done %u overruns %u skipped, outstanding %d",
          b.done, b.overruns, b.skipped, mo);
    CHECK(b.missed == 0, "overrun of the budget is not a deadline miss (5 ms of 1000)");

    /* overload: a step of 3.5 periods. No queue: the late releases are counted as skipped, never run back to back */
    ai_rt_t c = svc("bat", 1000, 1000, 2000);
    ai_rt_start(&c, 0);
    now = 0; mo = 0;
    simulate(&c, 1, 100000000, AI_DL_NORMAL, step_huge, &now, &mo);
    CHECK(mo == 1, "overload: never more than one job outstanding (%d)", mo);
    CHECK(c.released == c.done, "overload: every released job finished (%u / %u)", c.released, c.done);
    CHECK(c.released + c.skipped >= 99 && c.released + c.skipped <= 101, "overload: released %u + skipped %u = the 100 releases",
          c.released, c.skipped);
    CHECK(c.missed == c.done, "overload: every 3.5 s job missed its 1 s deadline (%u of %u)", c.missed, c.done);
    CHECK(c.done <= 30, "overload: the worker ran at most one job per 3.5 s (%u)", c.done);

    /* a spike every tenth job (1.5 s): one miss, one skip each time, the following jobs on time */
    ai_rt_t d = svc("bat", 1000, 1000, 2000);
    ai_rt_start(&d, 0);
    now = 0; mo = 0;
    simulate(&d, 1, 100000000, AI_DL_NORMAL, step_spike, &now, &mo);
    CHECK(mo == 1 && d.missed >= 8 && d.missed <= 10 && d.skipped >= d.missed, "spikes: %u missed, %u skipped, outstanding %d",
          d.missed, d.skipped, mo);

    /* the worker itself blocked for 5.5 s: on its return it runs the newest release (5 s) only, and counts 1..4 s as skipped */
    ai_rt_t e = svc("bat", 1000, 1000, 2000);
    ai_rt_start(&e, 0);
    int64_t w;
    CHECK(ai_rt_pick(&e, 1, 0, AI_DL_NORMAL, &w) == 0, "first release at start");
    ai_rt_finish(&e, 0, 300, AI_DL_NORMAL);
    CHECK(ai_rt_pick(&e, 1, 5500000, AI_DL_NORMAL, &w) == 0 && e.skipped == 4 && e.release_us == 5000000,
          "late worker: %u skipped, release %lld", e.skipped, (long long)e.release_us);
    ai_rt_finish(&e, 5500000, 5500300, AI_DL_NORMAL);
    CHECK(e.last_resp_us == 500300 && e.missed == 0, "late worker: response measured from the release (%u us)", e.last_resp_us);
    CHECK(ai_rt_pick(&e, 1, 5500300, AI_DL_NORMAL, &w) < 0 && w == 6000000, "next release on the grid (%lld)", (long long)w);

    /* deadline-monotonic: two due at once, the shorter deadline first */
    ai_rt_t two[2] = { svc("long", 1000, 1000, 1000), svc("short", 500, 200, 1000) };
    ai_rt_start(&two[0], 0); ai_rt_start(&two[1], 0);
    CHECK(ai_rt_pick(two, 2, 0, AI_DL_NORMAL, &w) == 1, "DM: the 200 ms deadline runs first");
    ai_rt_finish(&two[1], 0, 100, AI_DL_NORMAL);
    CHECK(ai_rt_pick(two, 2, 100, AI_DL_NORMAL, &w) == 0, "DM: then the 1000 ms one");
    ai_rt_finish(&two[0], 100, 200, AI_DL_NORMAL);
    uint32_t cc, tt, dd;
    ai_rt_worker_load(two, 2, &cc, &tt, &dd);
    CHECK(cc == 2000 && tt == 500 && dd == 200, "worker load: C %u T %u D %u", cc, tt, dd);

    /* slowed: period x10 */
    ai_rt_t s = svc("bat", 1000, 1000, 2000);
    ai_rt_start(&s, 0);
    now = 0; mo = 0;
    simulate(&s, 1, 100000000, AI_DL_SLOW, step_ok, &now, &mo);
    CHECK(s.released == 10 && s.skipped == 0, "slowed x10: %u releases in 100 s", s.released);

    /* paused: no release; resuming starts one period later, without counting the paused time as skipped */
    ai_rt_t p = svc("bat", 1000, 1000, 2000);
    ai_rt_start(&p, 0);
    now = 0; mo = 0;
    simulate(&p, 1, 60000000, AI_DL_PAUSED, step_ok, &now, &mo);
    CHECK(p.released == 0, "paused: no release (%u)", p.released);
    simulate(&p, 1, 70000000, AI_DL_NORMAL, step_ok, &now, &mo);
    CHECK(p.skipped == 0 && p.released >= 9 && p.released <= 10, "resumed: %u released, %u skipped", p.released, p.skipped);
}

/* the order the OS ladder sheds things in (os_core.c, the ladder with the AI rung first) and the AI level it implies */
static void test_degrade_order(void)
{
    os_ladder_t L = { 0 };
    uint32_t m = 0, ai = 0;
    uint8_t o = os_ladder_step(&L, m, ai, false);
    CHECK(o == OS_DL_NORMAL && ai_dl_from_os(o) == AI_DL_NORMAL, "primed: normal");
    o = os_ladder_step(&L, m += 3, ai, false);
    CHECK(o == OS_DL_AI_SLOW && ai_dl_from_os(o) == AI_DL_SLOW, "window 1: AI services slowed first, UI untouched (%u)", o);
    o = os_ladder_step(&L, m += 3, ai, false);
    CHECK(o == OS_DL_UI_SLOW && ai_dl_from_os(o) == AI_DL_PAUSED, "window 2: the UI slows, AI services paused (%u)", o);
    o = os_ladder_step(&L, m += 3, ai, false);
    CHECK(o == OS_DL_NO_LEARNER && ai_dl_from_os(o) == AI_DL_PAUSED, "window 3: learner paused (%u)", o);
    o = os_ladder_step(&L, m += 3, ai, false);
    CHECK(o == OS_DL_MODEL_OFF && ai_dl_from_os(o) == AI_DL_PAUSED, "window 4: model paused (%u)", o);
    int ai_slow_at = -1, ai_normal_at = -1, ui_back_at = -1;
    for (int w = 1; w <= 80; w++) {
        o = os_ladder_step(&L, m, ai, false);
        if (o <= OS_DL_AI_SLOW && ui_back_at < 0) ui_back_at = w;
        if (ai_dl_from_os(o) == AI_DL_SLOW && ai_slow_at < 0) ai_slow_at = w;
        if (ai_dl_from_os(o) == AI_DL_NORMAL && ai_normal_at < 0) ai_normal_at = w;
    }
    CHECK(ui_back_at == 18 && ai_slow_at == 18 && ai_normal_at == 24,
          "reverse order: UI back at window %d, AI slowed %d, AI normal %d (the services come back last)", ui_back_at, ai_slow_at, ai_normal_at);
    os_ladder_t U = { 0 };
    os_ladder_step(&U, 0, 0, false);
    for (int i = 0; i < 5; i++) o = os_ladder_step(&U, 0, ai += 2, false);
    CHECK(o == OS_DL_AI_SLOW, "AI-side misses only (UI, AI worker): the AI rung, nothing else sheds (%u)", o);
    CHECK(!strcmp(ai_dl_name(AI_DL_SLOW), "slowed x10"), "names");
}

static void test_snap(void)
{
    typedef struct { uint32_t a, b, c; float f; } blob_t;
    ai_seq_t sq = {0};
    blob_t shared = {0}, out = {0}, v = { 1, 2, 3, 4.5f };
    AI_SEQ_PUT(&sq, &shared, &v);
    CHECK(sq.seq == 2 && AI_SEQ_GET(&sq, &out, &shared) && out.c == 3 && out.f == 4.5f, "seqlock: write then read");
    /* a write in progress (odd sequence): the reader gives up after AI_SEQ_TRIES and keeps its last copy */
    ai_seq_write_begin(&sq);
    shared.a = 99;
    blob_t keep = out;
    CHECK(!AI_SEQ_GET(&sq, &out, &shared) && !memcmp(&out, &keep, sizeof out), "seqlock: torn read refused, last copy kept");
    ai_seq_write_end(&sq);
    CHECK(AI_SEQ_GET(&sq, &out, &shared) && out.a == 99, "seqlock: read after the write ends");

    static ai_ring_t r;
    memset(&r, 0, sizeof r);
    char line[32];
    for (int i = 0; i < AI_RING_SLOTS + 3; i++) { snprintf(line, sizeof line, "row %d", i); ai_ring_push(&r, line); }
    CHECK(ai_ring_count(&r) == AI_RING_SLOTS && r.dropped == 3, "ring: full at %d, 3 dropped (%u)", AI_RING_SLOTS, r.dropped);
    CHECK(!strcmp(ai_ring_peek(&r), "row 0"), "ring: oldest first");
    ai_ring_pop(&r);
    CHECK(ai_ring_push(&r, "new") && !strcmp(ai_ring_peek(&r), "row 1"), "ring: one slot freed, order kept");
    char big[300];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    while (ai_ring_peek(&r)) ai_ring_pop(&r);
    ai_ring_push(&r, big);
    CHECK(strlen(ai_ring_peek(&r)) == AI_RING_LINE - 1, "ring: a long line is cut, never overflows");
}

int main(void)
{
    test_release_table();
    test_degrade_order();
    test_snap();
    printf("ai_svc: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
