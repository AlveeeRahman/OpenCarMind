/* ai_svc: EdgeOS's small AI services on the ESP-Mosaico (CONFIG_OCM_AI_SVC). The battery estimator (bat_est.c) is the
 * first; a radio IDS classifier and others follow the same contract. The rules (release table, no pile-up, degrade
 * order) are in ai_svc_core.h; this file is the firmware side. See the block comment at the top of ai_svc.c for the
 * scheduling choice and the deadlock-freedom argument.
 *
 * A service is a table entry: name, period, relative deadline, CPU budget, and four hooks.
 *   step(release_us)  the worker task, once per release. Must never block: no lock, no I/O, no queue wait, no
 *                     allocation. Reads its inputs from single-writer snapshots (ai_svc_gauge_read), publishes its
 *                     result in a single-writer struct the UI reads lock-free, hands rows for storage to a ring.
 *   selftest()        board task, once before admission: one timed step on fixed data (us); over the budget = refused.
 *   prepare()         board task, once before the worker starts: may read NVS.
 *   board(shutdown)   board task, every 2 s and on the safe-shutdown path: may write NVS and files (rate-limited by
 *                     the service, except when shutdown is true).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "ai_svc_core.h"
#include "mosaico_board.h"

#define AI_SVC_PRIO 1                       /* below ui (2), rev (2), ml (3), obd (5); above idle (0) */

typedef struct {
    const char *name;                       /* short: the Board page and the sched line */
    uint32_t    period_ms, deadline_ms, budget_us;
    void        (*step)(int64_t release_us);
    uint32_t    (*selftest)(void);
    void        (*prepare)(void);
    void        (*board)(bool shutdown);
} ai_svc_def_t;

enum { AI_ST_OFF = 0, AI_ST_RUNNING, AI_ST_REFUSED, AI_ST_SAFE, AI_ST_NOTASK };

typedef struct {
    char     name[8];
    uint8_t  state;                         /* AI_ST_* */
    uint32_t period_ms, deadline_ms, budget_us;
    uint32_t released, done, missed, skipped, overruns, wcrt_us, max_step_us, selftest_us;
} ai_svc_stat_t;

typedef struct {
    uint8_t       level;                    /* AI_DL_* */
    int8_t        core;                     /* -1: no worker */
    uint8_t       n;
    char          why[72];                  /* "" while running, else why not (refusal, safe mode) */
    ai_svc_stat_t s[AI_SVC_MAX];
} ai_svc_status_t;

/* the gauge sample as the board task read it (no new I2C traffic), with its esp_timer time and a counter */
typedef struct { mosaico_gauge_t g; int64_t t_us; uint32_t n; } ai_gauge_sample_t;

#if CONFIG_OCM_AI_SVC
void ai_svc_start(void);                    /* board task, at the boot health verdict (os.c health_form) */
/* board task, every OS ladder window: the services' deadline misses so far (the ladder's AI-side input, with the
   ui and ai jobs'), logged once per window when they grew; then the ladder's new level sets the services' level */
uint32_t ai_svc_misses(void);
void     ai_svc_set_os_level(uint8_t os_level);
uint8_t  ai_svc_level(void);                /* AI_DL_*: a step may drop optional work ("learned heads") when slowed */
void ai_svc_board_tick(bool shutdown);      /* board task: every 2 s, and true on the safe-shutdown path */
void ai_svc_gauge_publish(const mosaico_gauge_t *g);   /* board task, after each gauge poll */
bool ai_svc_gauge_read(ai_gauge_sample_t *out);         /* any task, lock-free; false = no sample yet or torn */
bool ai_svc_status(ai_svc_status_t *out);  /* any task, lock-free */
int  ai_svc_log_line(char *buf, size_t n);  /* the sched summary's AI line; 0 = nothing to say */
#else
static inline uint8_t ai_svc_level(void) { return 0; }
static inline void ai_svc_start(void) {}
static inline void ai_svc_board_tick(bool shutdown) { (void)shutdown; }
static inline void ai_svc_gauge_publish(const mosaico_gauge_t *g) { (void)g; }
#endif
