#pragma once
/* App lists updated in place (R11, after UI1 measured app updates in the timed ui job up to 543.5 ms, the Files list
 * refill alone 177-185 ms of CPU). A list keeps its row objects for the whole visit; on an update each row's content
 * signature is compared with what the row shows, and only rows whose signature changed are rewritten. Rows past the new
 * count are hidden, never deleted, and rows are created only when the list grows past what was ever shown.
 * Pure C, no LVGL: host-tested in tests/host/test_ui_rows.c. */
#include <stdbool.h>
#include <stdint.h>

/* FNV-1a over bytes, chained: sig = urow_mix(urow_mix(UROW_SEED, &a, sizeof a), &b, sizeof b) */
#define UROW_SEED 2166136261u
uint32_t urow_mix(uint32_t h, const void *p, unsigned n);

enum { UROW_KEEP = 0, UROW_WRITE = 1, UROW_HIDE = 2, UROW_CREATE = 4 };
/* For each row i < max(shown, n): CREATE|WRITE when i >= built (a new object), WRITE when its signature changed or it
   was hidden, HIDE when i >= n and it was shown, else KEEP. shown_sig[] is updated to sig[] for the written rows.
   Returns how many rows are written (the cost of the update). */
int urow_plan(uint32_t *shown_sig, int *shown, int *built, const uint32_t *sig, int n, uint8_t *action, int max);
