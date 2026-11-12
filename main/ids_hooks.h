/* ids_hooks: what the radio callbacks write for the network guard, and how the IDS step reads it (OpenCarMind EdgeOS,
 * ESP-Mosaico). Header only, no IDF: host-tested in tests/host/test_ids.c, and tests/host/ids_audit.sh checks that the
 * hooks do nothing else (no lock, no allocation, no log, no event, no NVS, no radio call).
 *
 * One block per writer task, so a writer needs no lock and no atomic read-modify-write (DESIGN.md section 6.2):
 *   host   the NimBLE host task: adverts (a count and the address bitmaps), link up and down
 *   obd    the OBD task: the car link's response count and latency sum
 *   wev    the esp_event task: the Wi-Fi disconnect reasons that are deauth or disassoc
 *   wtask  the Wi-Fi sync task: rounds, scans (the BSSID bitmaps), our own disconnect
 * Counters are monotonic uint32_t, never reset; the reader keeps the last copy and subtracts (correct across the wrap).
 * The bitmaps are the one thing that must reset: IDS_BM_SLOTS slots indexed by the window number (BLE: uptime / 5 s;
 * Wi-Fi: the scan number), each tagged with its window + 1. The writer clears a slot when its window starts; the
 * reader copies a slot and keeps it only when its tag was the wanted one before and after the copy, so a slot that is
 * being reused is never read as data. Addresses are hashed (FNV-1a, mod IDS_ROT_BITS) and dropped in the hook: nothing
 * keeps or logs them.
 */
#ifndef IDS_HOOKS_H
#define IDS_HOOKS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "ids_model.h"

#define IDS_BM_WORDS (IDS_ROT_BITS / 32u)
#define IDS_BM_SLOTS 4u

typedef struct {
    uint32_t tag;                       /* window + 1; 0 = never used */
    uint32_t n;                         /* adverts (or AP records) in this window */
    uint32_t seen[IDS_BM_WORDS], twice[IDS_BM_WORDS];
} ids_bm_t;

typedef struct { uint32_t n_adv, link_up, link_down; ids_bm_t slot[IDS_BM_SLOTS]; } ids_host_blk_t;
typedef struct { uint32_t n_resp, sum16; } ids_obd_blk_t;
typedef struct { uint32_t deauth; } ids_wev_blk_t;
typedef struct { uint32_t rounds, leaving, scans_done, scan_seq; ids_bm_t slot[IDS_BM_SLOTS]; } ids_wtask_blk_t;

static inline uint32_t ids_fnv(const uint8_t *p, unsigned n, uint8_t type)
{
    uint32_t h = 2166136261u;
    h = (h ^ type) * 16777619u;
    for (unsigned i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static inline void ids_st(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline uint32_t ids_ld(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

/* writer: one address in window w (the slot is opened, cleared, on the window's first event) */
static inline void ids_bm_add(ids_bm_t *slots, uint32_t w, uint32_t h)
{
    ids_bm_t *b = &slots[w % IDS_BM_SLOTS];
    if (ids_ld(&b->tag) != w + 1u) {
        ids_st(&b->tag, 0);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memset(b->seen, 0, sizeof b->seen);
        memset(b->twice, 0, sizeof b->twice);
        ids_st(&b->n, 0);
        __atomic_store_n(&b->tag, w + 1u, __ATOMIC_RELEASE);
    }
    uint32_t k = h % IDS_ROT_BITS, word = k >> 5, bit = 1u << (k & 31u);
    if (b->seen[word] & bit) b->twice[word] |= bit;
    else b->seen[word] |= bit;
    ids_st(&b->n, b->n + 1u);
}

/* reader: copies window w's slot; false when it holds another window (no data in w, or reused meanwhile) */
static inline bool ids_bm_copy(const ids_bm_t *slots, uint32_t w, ids_bm_t *out)
{
    const ids_bm_t *b = &slots[w % IDS_BM_SLOTS];
    if (__atomic_load_n(&b->tag, __ATOMIC_ACQUIRE) != w + 1u) return false;
    memcpy(out, b, sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&b->tag, __ATOMIC_RELAXED) == w + 1u && out->tag == w + 1u;
}

static inline unsigned ids_pop(uint32_t x) { return (unsigned)__builtin_popcount(x); }

/* the rotation detector's inputs for window cur against the window before it (prev may be NULL: nothing seen) */
static inline void ids_bm_window(const ids_bm_t *cur, const ids_bm_t *prev, uint16_t *n_adv, uint16_t *set_bits,
                                 uint16_t *new_bits, uint16_t *su_new, uint16_t *pattern)
{
    unsigned s = 0, nw = 0, su = 0;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < IDS_BM_WORDS; i++) {
        uint32_t p = prev ? prev->seen[i] : 0u, fresh = cur->seen[i] & ~p, single = fresh & ~cur->twice[i];
        s += ids_pop(cur->seen[i]);
        nw += ids_pop(fresh);
        su += ids_pop(single);
        if (single) h = (h ^ (single ^ i)) * 16777619u;
    }
    *n_adv = (uint16_t)(cur->n > 0xFFFFu ? 0xFFFFu : cur->n);
    *set_bits = (uint16_t)s;
    *new_bits = (uint16_t)nw;
    *su_new = (uint16_t)su;
    if (pattern) *pattern = (uint16_t)(h ^ (h >> 16));
}

/* the Wi-Fi disconnect reasons that are a deauth or a disassoc (802.11 reason codes 2, 3, 6, 7, 8) */
static inline bool ids_reason_deauth(int r) { return r == 2 || r == 3 || r == 6 || r == 7 || r == 8; }

#endif
