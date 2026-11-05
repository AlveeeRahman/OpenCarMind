/* Freshness of decoded OBD fields, see obd_fresh.h. */
#include "obd_fresh.h"

static bool bit(const uint64_t s[2], int i) { return (s[i / 64] >> (i % 64)) & 1ULL; }

int obd_fresh_expire(uint64_t valid[2], const uint64_t fast[2], const uint32_t *t_ms, int n, uint32_t now_ms,
                     uint32_t cycle_ms, uint32_t slow_cycles)
{
    if (slow_cycles < 1) slow_cycles = 1;
    uint32_t fast_lim = 3u * cycle_ms, slow_lim = 3u * cycle_ms * slow_cycles;
    if (fast_lim < OBD_FRESH_FAST_MIN_MS) fast_lim = OBD_FRESH_FAST_MIN_MS;
    if (slow_lim < OBD_FRESH_SLOW_MIN_MS) slow_lim = OBD_FRESH_SLOW_MIN_MS;
    int cleared = 0;
    for (int i = 0; i < n && i < 128; i++) {
        if (!bit(valid, i)) continue;
        uint32_t age = now_ms - t_ms[i];                      /* unsigned difference survives the 49-day wrap */
        if (age > (bit(fast, i) ? fast_lim : slow_lim)) {
            valid[i / 64] &= ~(1ULL << (i % 64));
            cleared++;
        }
    }
    return cleared;
}
