#include "ui_rows_logic.h"

uint32_t urow_mix(uint32_t h, const void *p, unsigned n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (unsigned i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

int urow_plan(uint32_t *shown_sig, int *shown, int *built, const uint32_t *sig, int n, uint8_t *action, int max)
{
    if (n > max) n = max;
    if (n < 0) n = 0;
    int top = n > *shown ? n : *shown, writes = 0;
    for (int i = 0; i < top; i++) {
        if (i >= n) { action[i] = UROW_HIDE; continue; }
        if (i >= *built) { action[i] = UROW_CREATE | UROW_WRITE; shown_sig[i] = sig[i]; writes++; continue; }
        if (i >= *shown || shown_sig[i] != sig[i]) { action[i] = UROW_WRITE; shown_sig[i] = sig[i]; writes++; continue; }
        action[i] = UROW_KEEP;
    }
    if (n > *built) *built = n;
    *shown = n;
    return writes;
}
