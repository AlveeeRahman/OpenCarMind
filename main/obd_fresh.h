/* Freshness of decoded OBD fields. Pure C, host-tested (tests/host/test_main.c).
 *
 * A field's valid bit used to mean "read at least once in this drive", so a PID that stopped answering kept its last value,
 * flagged valid, until the drive ended: the fuel picker kept using a frozen MAF and R.E.V learned from it. Now each field has
 * the time of its last good read, and a field that has not been read for longer than its tier allows is marked missing:
 *   - fast tier (read every cycle): missing after max(OBD_FRESH_FAST_MIN_MS, 3 cycles);
 *   - slow tier (round robin, a few per cycle): missing after max(OBD_FRESH_SLOW_MIN_MS, 3 trips round the slow list).
 * The next good read sets the bit again. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define OBD_FRESH_FAST_MIN_MS 4000u
#define OBD_FRESH_SLOW_MIN_MS 10000u

/* Clear the valid bit of every field older than its limit. valid and fast are bit sets of n fields (n <= 128); t_ms[i] is the
   time of field i's last good read; cycle_ms is the typical poll cycle; slow_cycles is how many cycles one trip round the
   slow list takes (>= 1). Returns how many fields were cleared. now_ms and t_ms share a clock that may wrap. */
int obd_fresh_expire(uint64_t valid[2], const uint64_t fast[2], const uint32_t *t_ms, int n, uint32_t now_ms,
                     uint32_t cycle_ms, uint32_t slow_cycles);
