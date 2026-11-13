/* stop_core.c: the stop-logging state machine (OpenCarMind EdgeOS 2.1.1 field fix) with injected failures and timeouts.
 * A fake board answers the inputs: the poll loop, the session files, the storage and the background writers, each with a
 * fault switch. Every stop must end READY (verified) or FAILED inside the step limits, never READY over unsaved data, and
 * logging must stay off until stop_release. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stop_core.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct {
    /* the board */
    bool poll_running, session_open, unsaved, paused, storage_ok, halted;
    int64_t last_write, now;
    int restarts;                 /* drives started while halted: must stay 0 */
    /* faults */
    int64_t poll_stops_at;        /* the poll loop leaves the car link at this time (INT64_MAX = never) */
    int close_fails;              /* this many close attempts fail (storage busy), -1 = always */
    bool writer_ignores_pause;    /* the battery log keeps writing every second */
    int64_t write_at;             /* one extra write at this time (0 = none) */
    bool car_answers;             /* the car still answers: an unlatched link would start the next drive */
} board_t;

static void board_init(board_t *b)
{
    memset(b, 0, sizeof *b);
    b->poll_running = true; b->session_open = true; b->unsaved = true; b->storage_ok = true;
    b->poll_stops_at = 2000; b->car_answers = true; b->now = 0; b->last_write = 0;
}

static void apply(board_t *b, unsigned act)
{
    if (act & STOP_DO_HALT) b->halted = true;
    if (act & STOP_DO_RELEASE) { b->halted = false; b->paused = false; }
    if ((act & STOP_DO_CLOSE) && !b->poll_running) {
        if (b->close_fails == 0) { b->session_open = false; b->unsaved = false; b->last_write = b->now; }
        else if (b->close_fails > 0) b->close_fails--;
    }
    if (act & STOP_DO_PAUSE) b->paused = true;
    if ((act & STOP_DO_SYNC) && b->storage_ok) b->unsaved = false;
}

static void tick_board(board_t *b)
{
    if (b->poll_running && b->halted && b->now >= b->poll_stops_at) b->poll_running = false;
    if (b->poll_running) { b->unsaved = true; b->last_write = b->now; }           /* rows */
    if (!b->poll_running && !b->halted && b->car_answers) { b->poll_running = true; b->session_open = true; }
    if (!b->poll_running && b->halted && b->car_answers && b->now % 5000 == 0 && b->now > b->poll_stops_at) {
        /* the obd task's 5 s retry: with the latch held it must not start a drive */
        if (!b->halted) b->restarts++;
    }
    if ((b->writer_ignores_pause || !b->paused) && b->now % 1000 == 0 && b->now > 0) { b->unsaved = true; b->last_write = b->now; }
    if (b->write_at && b->now == b->write_at) { b->unsaved = true; b->last_write = b->now; }
}

static stop_in_t inputs(const board_t *b, bool closed_ok)
{
    stop_in_t in = { b->poll_running, b->session_open, closed_ok, !b->unsaved, b->now - b->last_write, b->paused, b->storage_ok };
    return in;
}

/* runs until READY/FAILED or `limit` ms; returns the state, checks the invariants at every step */
static stop_state_t run(stop_ctl_t *c, board_t *b, int64_t limit, int64_t *took)
{
    int64_t t0 = b->now;
    while (b->now - t0 < limit) {
        b->now += 100;
        tick_board(b);
        stop_in_t in = inputs(b, !b->session_open);
        unsigned act = stop_step(c, &in, b->now);
        if (c->st == STOP_READY) {
            /* READY is only ever shown over a verified state */
            CHECK(!in.poll_running && !in.session_open && in.saved && in.writers_paused && in.ms_since_write >= STOP_QUIET_MS);
        }
        if (c->latched) CHECK(act & STOP_DO_HALT);
        apply(b, act);
        if (c->st == STOP_READY || c->st == STOP_FAILED) { if (took) *took = b->now - c->t_req; return c->st; }
    }
    if (took) *took = b->now - t0;
    return c->st;
}

int main(void)
{
    stop_ctl_t c;
    board_t b;
    int64_t took;
    const int64_t BOUND = STOP_LINK_MS + STOP_CLOSE_MS + STOP_SYNC_MS + 200;

    /* 1. the normal stop while the car still answers: READY in a few seconds, and no new drive starts */
    stop_init(&c); board_init(&b);
    apply(&b, stop_request(&c, b.now));
    CHECK(c.st == STOP_LINK && c.latched && b.halted);
    CHECK(run(&c, &b, 60000, &took) == STOP_READY);
    CHECK(took < 6000);
    CHECK(!b.poll_running && !b.session_open);
    /* stays READY: the car keeps answering, no restart, for 10 minutes */
    for (int i = 0; i < 6000; i++) {
        b.now += 100; tick_board(&b);
        stop_in_t in = inputs(&b, true);
        unsigned act = stop_step(&c, &in, b.now);
        apply(&b, act);
        CHECK(act & STOP_DO_HALT);
        if (c.st != STOP_READY) { CHECK(0); break; }
    }
    CHECK(!b.poll_running && b.restarts == 0);
    CHECK(strcmp(stop_state_text(&c), "Logging stopped. Safe to unplug.") == 0);
    /* a second request while READY changes nothing */
    CHECK(stop_request(&c, b.now) == STOP_DO_HALT && c.st == STOP_READY);
    /* Start logging: back to IDLE, the latch is gone, the drive may start */
    CHECK(stop_release(&c, b.now) == STOP_DO_RELEASE);
    apply(&b, STOP_DO_RELEASE);
    CHECK(c.st == STOP_IDLE && !c.latched);
    b.now += 100; tick_board(&b);
    CHECK(b.poll_running);
    CHECK(stop_release(&c, b.now) == 0);

    /* 2. the poll loop never stops (a stuck link): FAILED with the link reason in STOP_LINK_MS, logging still held */
    stop_init(&c); board_init(&b); b.poll_stops_at = INT64_MAX;
    apply(&b, stop_request(&c, b.now));
    CHECK(run(&c, &b, 60000, &took) == STOP_FAILED);
    CHECK(c.why == STOP_WHY_LINK && c.failed_at == STOP_LINK);
    CHECK(took >= STOP_LINK_MS && took <= STOP_LINK_MS + 200);
    CHECK(c.latched && b.halted);
    CHECK(strcmp(stop_why_text(c.why), "The car link did not stop") == 0);
    /* Try again once the link lets go: READY */
    b.poll_stops_at = b.now + 1000;
    apply(&b, stop_retry(&c, b.now));
    CHECK(c.st == STOP_LINK);
    CHECK(run(&c, &b, 60000, &took) == STOP_READY);

    /* 3. storage busy: the close fails for longer than STOP_CLOSE_MS, FAILED with the close reason, then a retry works */
    stop_init(&c); board_init(&b); b.close_fails = -1;
    apply(&b, stop_request(&c, b.now));
    CHECK(run(&c, &b, 60000, &took) == STOP_FAILED);
    CHECK(c.why == STOP_WHY_CLOSE && c.failed_at == STOP_CLOSE);
    CHECK(took <= b.poll_stops_at + STOP_CLOSE_MS + 300);
    CHECK(c.tries > 50);                                         /* it kept trying, every step */
    b.close_fails = 3;
    apply(&b, stop_request(&c, b.now));                          /* a new request from FAILED is a retry */
    CHECK(c.st == STOP_CLOSE);
    CHECK(run(&c, &b, 60000, &took) == STOP_READY);

    /* 4. a few failed closes inside the limit: READY, no failure shown */
    stop_init(&c); board_init(&b); b.close_fails = 20;
    apply(&b, stop_request(&c, b.now));
    CHECK(run(&c, &b, 60000, &took) == STOP_READY);
    CHECK(took < BOUND);

    /* 5. a writer that ignores the pause (writes every second): never READY, FAILED with the sync reason */
    stop_init(&c); board_init(&b); b.writer_ignores_pause = true;
    apply(&b, stop_request(&c, b.now));
    CHECK(run(&c, &b, 60000, &took) == STOP_FAILED);
    CHECK(c.why == STOP_WHY_SYNC && took < BOUND);

    /* 6. one late write after READY (a learned-state save): back to SYNC, READY again after the quiet time */
    stop_init(&c); board_init(&b);
    apply(&b, stop_request(&c, b.now));
    CHECK(run(&c, &b, 60000, &took) == STOP_READY);
    b.write_at = b.now + 3000;
    bool saw_sync = false;
    for (int i = 0; i < 100; i++) {
        b.now += 100; tick_board(&b);
        stop_in_t in = inputs(&b, true);
        apply(&b, stop_step(&c, &in, b.now));
        if (c.st == STOP_SYNC) saw_sync = true;
        if (c.st == STOP_READY) CHECK(in.saved && in.ms_since_write >= STOP_QUIET_MS);
    }
    CHECK(saw_sync && c.st == STOP_READY);

    /* 7. no storage: FAILED, the reason says nothing could be saved */
    stop_init(&c); board_init(&b); b.storage_ok = false;
    apply(&b, stop_request(&c, b.now));
    CHECK(run(&c, &b, 60000, &took) == STOP_FAILED);
    CHECK(c.why == STOP_WHY_STORAGE);

    /* 8. no drive running (parked, Settings > Stop logging): READY straight away */
    stop_init(&c); board_init(&b); b.poll_running = false; b.session_open = true; b.car_answers = false;
    apply(&b, stop_request(&c, b.now));
    CHECK(run(&c, &b, 60000, &took) == STOP_READY);
    CHECK(took < 3000);

    /* 9. random faults, 20,000 runs: every stop ends READY or FAILED inside the bound, READY only over a verified state
          (checked inside run), and the latch never lets a drive start */
    srand(7);
    int ready = 0, failed = 0;
    for (int k = 0; k < 20000; k++) {
        stop_init(&c); board_init(&b);
        b.poll_stops_at = rand() % 4 == 0 ? INT64_MAX : (rand() % 20000);
        b.close_fails = rand() % 5 == 0 ? -1 : rand() % 150;
        b.writer_ignores_pause = rand() % 10 == 0;
        b.storage_ok = rand() % 50 != 0;
        b.car_answers = rand() % 2;
        b.poll_running = rand() % 5 != 0;
        apply(&b, stop_request(&c, b.now));
        stop_state_t s = run(&c, &b, 4 * BOUND, &took);
        CHECK(s == STOP_READY || s == STOP_FAILED);
        CHECK(took <= BOUND);
        CHECK(b.restarts == 0 && c.latched);
        if (s == STOP_READY) ready++; else failed++;
    }
    CHECK(ready > 1000 && failed > 1000);
    printf("stop: %d ready, %d failed in the random runs\n", ready, failed);

    printf("%d checks, %d failures\n", runs, fails);
    return fails != 0;
}
