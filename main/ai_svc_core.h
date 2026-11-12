/* ai_svc_core: the pure rules of EdgeOS's small AI services (ESP-Mosaico). No IDF: host-tested in
 * tests/host/test_ai_svc.c. The firmware glue is ai_svc.c.
 *
 * 1. Release table. Every service is a periodic job: period T, relative deadline D <= T, a WCET budget C. One worker
 *    task per board runs them (ai_svc.c says why one and not one task each). When several are due at once, the one
 *    with the shortest D runs first (deadline-monotonic, Leung and Whitehead 1982; equal to rate-monotonic when D = T).
 *    Steps are short and never block, so inside the worker they do not preempt each other; every other task with a
 *    deadline (UI, models, OBD) has a higher FreeRTOS priority and preempts the worker at any instruction.
 * 2. No pile-up. At most one job of a service is ever outstanding. A worker that comes back late, after k more
 *    releases have passed, runs only the newest one and counts the k older ones as skipped (they are never queued).
 *    A step that runs longer than its budget finishes (it is never killed mid-update), and the service's next release
 *    is skipped, so an overrun cannot push the following jobs into a backlog either.
 * 3. Soft real time. A job whose response time (finish minus release) exceeds D is counted as missed. A miss is never
 *    fatal: it feeds the degradation rule below.
 * 4. Degradation (AI services shed first). The OS ladder (os_core.h section 7) has the AI rung first: at rung 1 every
 *    service runs at period x10 with its learned heads off, from rung 2 (UI slowed) up the services are paused. The
 *    services' own deadline misses, the UI's and the AI worker's feed the ladder's AI-side input, which can climb to
 *    rung 1 only; model misses climb every rung. The rungs come back in reverse order, so the services come back last.
 */
#ifndef AI_SVC_CORE_H
#define AI_SVC_CORE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_SVC_MAX        4
#define AI_DL_SLOW_FACTOR 10
enum { AI_DL_NORMAL = 0, AI_DL_SLOW, AI_DL_PAUSED };

typedef struct {
    const char *name;
    uint32_t    period_ms, deadline_ms, budget_us;
    bool        active;                   /* admitted and running */
    int64_t     next_us;                  /* the next release instant */
    int64_t     release_us;               /* release of the job picked last */
    uint32_t    released, done, missed, skipped, overruns;
    uint32_t    wcrt_us, last_resp_us, max_step_us;
} ai_rt_t;

/* the first release at now_us */
void     ai_rt_start(ai_rt_t *t, int64_t now_us);
uint64_t ai_rt_period_us(const ai_rt_t *t, uint8_t level);
/* The due service with the shortest deadline (index), or -1 when none is due. *wake_us: the earliest next release
   among the active services (now + 1 s while paused). Counts the releases a late worker passed over as skipped. */
int      ai_rt_pick(ai_rt_t *t, int n, int64_t now_us, uint8_t level, int64_t *wake_us);
/* the picked job ran from start_us to end_us: response, miss, overrun (skip the next release) */
void     ai_rt_finish(ai_rt_t *t, int64_t start_us, int64_t end_us, uint8_t level);

/* the AI level for an OS ladder level: rung 0 normal, rung 1 slowed, rung 2 and up paused */
uint8_t     ai_dl_from_os(uint8_t os_level);
const char *ai_dl_name(uint8_t level);

/* the admission candidate of a worker that runs services[0..n) (active ones only): C = the sum of the budgets (all can
   be released at the same instant), T = the shortest period, D = the shortest deadline */
void ai_rt_worker_load(const ai_rt_t *t, int n, uint32_t *c_us, uint32_t *t_ms, uint32_t *d_ms);

#ifdef __cplusplus
}
#endif
#endif
