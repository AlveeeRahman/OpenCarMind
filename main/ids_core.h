/* ids_core: the network guard's pure rules (OpenCarMind EdgeOS AI service "ids", ESP-Mosaico). No IDF: host-tested in
 * tests/host/test_ids.c. The firmware glue (the radio hooks, the service step, the consumers) is ids_svc.c.
 *
 * Three heads, one 5 s window at a time (carmind-ml experiments/net_ids: DESIGN.md section 6, UNSUPERVISED.md):
 *   scan  the BLE adverts the scan sees: ids_model.h's rotation detector (single-use new transmitters, churn, adverts per
 *         address; static prior + learnt robust z, 3 of 6 windows, poisoning guard). It blocks.
 *   link  the car link's response time (mean ELM round trip per window): frugal median and MAD of this board's normal,
 *         one-sided robust z over IDS_LINK_Z, 3 of 6 windows. Advisory only (relay timing found +80 % reliably, +40 %
 *         partly, from 1.5 h of data): it marks rows and shows a note, it never blocks.
 *   wifi  per Wi-Fi scan in a clock round: the same rotation detector, static prior only (a round gives a few windows,
 *         so it could never learn), its persistence reset at every round; plus the deauth rule: IDS_DEAUTH_K or more
 *         deauth or disassoc disconnects in one round. Either blocks the round.
 *
 * Verdict and states (what the Health app shows):
 *   learning  no head has seen a window yet
 *   shadow    the first IDS_SHADOW_WIN windows (2 h) the guard runs: heads learn and score, nothing is acted on; a block
 *             they would raise is logged as "would block"
 *   watching  after the shadow, no block
 *   blocking  a head blocks and the user's "Network guard: blocking" setting is on. With the setting off the heads still
 *             run and the "would block" events are logged.
 * A block lifts by itself IDS_HOLD_WIN windows (10 min) after the head's last anomalous window, or at once with the
 * user's Clear (the learnt baselines are kept). What blocking does is the consumers' business (ids_svc.h): no pairing
 * prompts or re-pair requests, the scan narrowed to the paired adapter, the Wi-Fi round stopped with the clock kept.
 * Never: cut the paired link, transmit, pair, change the clock. Addresses never leave the hooks: only bitmap bits and a
 * 16-bit hash prefix of the window's pattern.
 */
#ifndef IDS_CORE_H
#define IDS_CORE_H
#include <stdbool.h>
#include <stdint.h>
#include "ids_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IDS_WIN_MS      5000u
#define IDS_SHADOW_WIN  1440u          /* 2 h of 5 s windows */
#define IDS_HOLD_WIN    IDS_ROT_HOLD   /* 10 min */
#define IDS_DEAUTH_K    3
#define IDS_LINK_Z      6
#define IDS_LINK_FLOOR  32             /* 2 ms, in 1/16 ms */
#define IDS_LINK_WARM   60             /* windows with link data before the link head scores */
#define IDS_LINK_MIN_N  3              /* responses in a window */
#define IDS_LINK_CLEAN  6              /* clean windows in a row end the link note */

enum { IDS_ST_OFF = 0, IDS_ST_LEARNING, IDS_ST_SHADOW, IDS_ST_WATCHING, IDS_ST_BLOCKING };
enum { IDS_R_NONE = 0, IDS_R_SCAN, IDS_R_WIFI_ROT, IDS_R_DEAUTH };
enum { IDS_H_SCAN = 0, IDS_H_LINK, IDS_H_WIFI, IDS_NH };

/* what changed in one step (the service turns each into an event) */
enum {
    IDS_CH_SHADOW_END = 1u << 0,
    IDS_CH_BLOCK_ON   = 1u << 1,       /* a real block started (reason in ids_t.reason) */
    IDS_CH_BLOCK_OFF  = 1u << 2,       /* it lifted by itself */
    IDS_CH_CLEARED    = 1u << 3,       /* the user cleared it */
    IDS_CH_WOULD      = 1u << 4,       /* a head would block, but the shadow or the setting holds it */
    IDS_CH_LINK_ON    = 1u << 5,       /* the link head's note started */
    IDS_CH_LINK_OFF   = 1u << 6,
};

typedef struct { uint16_t n_adv, set_bits, new_bits, su_new; bool on; } ids_win_t;

typedef struct {
    int32_t  med, mad;                 /* 1/16 ms */
    uint32_t accepted, last_exc, windows, clean;
    uint8_t  hist;
    bool     init, flag;
} ids_link_t;

typedef struct {
    ids_win_t scan;                    /* the newest complete 5 s window of the scan (on: its previous window had data too) */
    ids_win_t wifi[2];                 /* Wi-Fi scans finished since the last step, oldest first */
    uint8_t   n_wifi;
    bool      round_start;             /* a new clock round started since the last step */
    uint16_t  deauth_round;            /* deauth or disassoc disconnects in the current round */
    uint32_t  link_n, link_sum16;      /* car-link responses since the last step and their latency sum (1/16 ms) */
    uint8_t   nwin;                    /* 5 s windows this step covers: 1, or the slow factor on the ladder's AI rung */
    bool      clear;                   /* the user's Clear */
    bool      blocking_on;             /* the setting */
    uint32_t  day;                     /* the local day number (0 = no clock): "blocks today" resets on a change */
} ids_in_t;

typedef struct {
    ids_rot_t  scan, wifi;
    ids_link_t link;
    uint32_t   win;                    /* 5 s windows since the guard started */
    uint32_t   wifi_until;             /* the Wi-Fi block lasts until this window */
    uint32_t   scan_exc_win;           /* the window of the scan head's last exceedance (the hold is in wall time) */
    uint8_t    state, reason, wifi_reason;
    bool       scan_block, wifi_block, acting, would, seen;
    int32_t    q8[IDS_NH];             /* the heads' newest scores (256 = threshold) */
    bool       on[IDS_NH];
    uint32_t   blocks_today, day, would_n, scan_windows, wifi_scans;
} ids_t;

void     ids_init(ids_t *g);
uint32_t ids_step(ids_t *g, const ids_in_t *in);          /* IDS_CH_* bits */
bool     ids_shadow(const ids_t *g);
uint32_t ids_block_left_s(const ids_t *g);                /* while blocking: until it lifts by itself */
const char *ids_reason_text(uint8_t r);                   /* "radio flood nearby", "Wi-Fi beacon flood", "Wi-Fi deauth attack" */
const char *ids_state_name(uint8_t st);
/* the deauth rule alone, for the Wi-Fi task's inline check before the clock is set */
static inline bool ids_deauth_rule(uint16_t deauth_round) { return deauth_round >= IDS_DEAUTH_K; }
/* the link head on one window's mean latency (1/16 ms): the score (Q8), and the note's start (true once) */
int32_t  ids_link_score(const ids_link_t *l, int32_t x);
bool     ids_link_step(ids_link_t *l, int32_t x, bool *ended);

#ifdef __cplusplus
}
#endif
#endif
