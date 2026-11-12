/* ui_rows_logic.c: lists updated in place. A model "screen" applies each plan; after every update it must show exactly
   the new list, and the number of rows written must equal the number of rows whose content changed (plus new ones). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_rows_logic.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define MAXR 48

typedef struct { uint32_t id, bytes; } item_t;
static uint32_t sig_of(const item_t *it) { return urow_mix(urow_mix(UROW_SEED, &it->id, 4), &it->bytes, 4); }

int main(void)
{
    uint32_t shown_sig[MAXR], sig[MAXR];
    int shown = 0, built = 0;
    uint8_t act[MAXR];
    item_t screen[MAXR]; int visible[MAXR], objects = 0;
    item_t list[MAXR], prev[MAXR];
    int n = 0, prevn = 0;
    srand(7);
    long total_writes = 0, total_changed = 0;
    int ok_all = 1;
    for (int round = 0; round < 3000; round++) {
        memcpy(prev, list, sizeof list); prevn = n;
        int op = rand() % 6;
        if (op == 0 && n < MAXR) { memmove(&list[1], &list[0], (size_t)n * sizeof list[0]); list[0] = (item_t){ (uint32_t)(1000 + round), 0 }; n++; }   /* a new drive on top */
        else if (op == 1 && n) list[0].bytes += 512;                                     /* the drive being written grows */
        else if (op == 2 && n) { int k = rand() % n; memmove(&list[k], &list[k + 1], (size_t)(n - k - 1) * sizeof list[0]); n--; }   /* one deleted */
        else if (op == 3 && n > 3) n = rand() % 3;                                       /* delete all */
        else if (op == 4) { }                                                            /* nothing changed */
        else if (op == 5) n = n;                                                         /* a rescan with the same list */
        for (int i = 0; i < n; i++) sig[i] = sig_of(&list[i]);
        int changed = 0;
        for (int i = 0; i < n; i++) changed += i >= prevn || sig_of(&prev[i]) != sig[i];
        int hidden_before = 0;
        for (int i = n; i < prevn; i++) hidden_before++;
        int w = urow_plan(shown_sig, &shown, &built, sig, n, act, MAXR);
        int top = n > prevn ? n : prevn;
        for (int i = 0; i < top; i++) {
            if (act[i] & UROW_CREATE) { CHECK(i == objects); objects++; }
            if (act[i] & UROW_WRITE) { screen[i] = list[i]; visible[i] = 1; }
            if (act[i] == UROW_HIDE) visible[i] = 0;
        }
        int same = 1;
        for (int i = 0; i < n; i++) same &= visible[i] && screen[i].id == list[i].id && screen[i].bytes == list[i].bytes;
        for (int i = n; i < objects; i++) same &= !visible[i] || i >= top;
        if (!same || w != changed) ok_all = 0;
        total_writes += w; total_changed += changed;
        (void)hidden_before;
    }
    CHECK(ok_all);
    CHECK(total_writes == total_changed);
    CHECK(built <= MAXR && built >= shown);
    /* the case UI1 measured: a 48-row list where one row (the drive being written) changes: 1 row written, not 48 */
    shown = built = 0;
    for (int i = 0; i < MAXR; i++) { list[i] = (item_t){ (uint32_t)i, 100 }; sig[i] = sig_of(&list[i]); }
    CHECK(urow_plan(shown_sig, &shown, &built, sig, MAXR, act, MAXR) == MAXR);   /* first fill: every row */
    list[0].bytes = 200; sig[0] = sig_of(&list[0]);
    CHECK(urow_plan(shown_sig, &shown, &built, sig, MAXR, act, MAXR) == 1 && act[0] == UROW_WRITE && act[1] == UROW_KEEP);
    CHECK(urow_plan(shown_sig, &shown, &built, sig, MAXR, act, MAXR) == 0);      /* nothing changed: nothing written */
    CHECK(urow_plan(shown_sig, &shown, &built, sig, 2, act, MAXR) == 0 && act[2] == UROW_HIDE && act[47] == UROW_HIDE);
    CHECK(urow_plan(shown_sig, &shown, &built, sig, 5, act, MAXR) == 3 && act[2] == UROW_WRITE && !(act[2] & UROW_CREATE));  /* shown again, not recreated */
    CHECK(urow_plan(shown_sig, &shown, &built, sig, 99, act, MAXR) == MAXR - 5 && shown == MAXR);   /* clamped to max */
    printf("ui_rows: %d checks, %d failed (3000 random updates: %ld rows written for %ld changed)\n", runs, fails, total_writes, total_changed);
    return fails != 0;
}
