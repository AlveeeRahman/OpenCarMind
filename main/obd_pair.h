/* obd_pair: the pure rules for choosing and pairing a Bluetooth LE OBD adapter (owner, 2026-11-11: "ask + strong signal").
 * No IDF, no heap, no clock of its own: host-tested in tests/host/test_obd_pair.c. obd_ble.c keeps one obd_pair_t under a
 * lock and does what these rules return (connect, drop the link, write NVS, delete a bond).
 *
 * Choosing a device from one advert (obd_pair_pick):
 *   - the saved adapter is a candidate down to the link floor (CONFIG_OBD_BLE_MIN_RSSI, -90 dBm), so a paired adapter still
 *     works at the edge of range, and it needs no question;
 *   - any other device is only ever OFFERED for pairing, and only when all of these hold: no adapter is saved, or the user
 *     started Find OBD (the automatic search never looks past a saved adapter); it looks like an OBD adapter (name or
 *     serial service); its signal is at or above the pairing floor (CONFIG_OBD_BLE_PAIR_MIN_RSSI, -70 dBm); it is not
 *     skipped; and no other offer is open (one question at a time).
 *
 * Pairing a new device (one at a time):
 *   IDLE -> PROBING   connected to it: the serial line and the ELM327 reset are tried. Nothing is logged or saved.
 *   PROBING -> ASKING it answered the reset like an ELM327: the caller drops the link at once and the user is asked.
 *   PROBING -> IDLE   it did not answer like an adapter (the caller's failure skip list takes it).
 *   ASKING -> IDLE    "Use this adapter" (OBD_ANS_YES) before the ask time ends: SAVE, the only path that saves.
 *                     "Not this one" (OBD_ANS_NO): skipped for this boot.
 *                     no answer within the ask time: not saved, skipped until the user's next Find OBD (or the next boot).
 * A skipped device is never offered again while it is skipped. The skip list holds OBD_PAIR_NSKIP devices; when it is full
 * the oldest entry is replaced.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OBD_PAIR_KEY_LEN   7       /* address type + 6 address bytes: compared, never shown or logged */
#define OBD_PAIR_NAME_LEN  24
#define OBD_PAIR_NSKIP     8
#define OBD_PAIR_STRONG_DBM (-60)  /* at or above: "strong", below (down to the pairing floor): "good" */

typedef enum { OBD_PAIR_IDLE = 0, OBD_PAIR_PROBING, OBD_PAIR_ASKING } obd_pair_phase_t;
typedef enum { OBD_PICK_NONE = 0, OBD_PICK_SAVED, OBD_PICK_OFFER } obd_pick_t;
typedef enum { OBD_ANS_YES = 0, OBD_ANS_NO } obd_answer_t;
typedef enum { OBD_DONE_NONE = 0, OBD_DONE_SAVE, OBD_DONE_DECLINED, OBD_DONE_EXPIRED } obd_done_t;
enum { OBD_SKIP_DECLINED = 1, OBD_SKIP_EXPIRED = 2 };

typedef struct {
    const uint8_t *key;    /* OBD_PAIR_KEY_LEN bytes */
    int  rssi;             /* dBm */
    bool have_saved;       /* an adapter is saved */
    bool is_saved;         /* this advert is the saved adapter */
    bool user_search;      /* the user's Find OBD window is open */
    bool looks_obd;        /* name prefix, OBD-looking name or an OBD serial service */
    bool failed;           /* on the caller's failure skip list (did not answer like an adapter) */
} obd_adv_t;

typedef struct {
    obd_pair_phase_t phase;
    uint8_t  key[OBD_PAIR_KEY_LEN];
    char     name[OBD_PAIR_NAME_LEN];
    int      rssi;
    int64_t  until_ms;             /* ASKING: the answer must come before this */
    uint32_t seq;                  /* +1 for every new question */
    struct { uint8_t key[OBD_PAIR_KEY_LEN]; uint8_t why; } skip[OBD_PAIR_NSKIP];
    uint8_t  skip_next;
} obd_pair_t;

typedef struct {
    obd_done_t done;
    uint8_t    key[OBD_PAIR_KEY_LEN];
    char       name[OBD_PAIR_NAME_LEN];
} obd_pair_result_t;

void        obd_pair_init(obd_pair_t *p);
obd_pick_t  obd_pair_pick(const obd_pair_t *p, const obd_adv_t *a, int link_min_dbm, int pair_min_dbm);
bool        obd_pair_skipped(const obd_pair_t *p, const uint8_t *key);
/* connected to an offered device: PROBING. The name is kept printable ASCII only (the screen fonts have nothing else). */
void        obd_pair_probe(obd_pair_t *p, const uint8_t *key, const char *name, int rssi);
bool        obd_pair_is_probing(const obd_pair_t *p, const uint8_t *key);
void        obd_pair_probe_failed(obd_pair_t *p);
/* the probed device answered the ELM327 reset: ASKING until now + ask_ms. False (no change) unless it was PROBING. */
bool        obd_pair_probe_answered(obd_pair_t *p, int64_t now_ms, uint32_t ask_ms);
obd_pair_result_t obd_pair_answer(obd_pair_t *p, obd_answer_t a, int64_t now_ms);
obd_pair_result_t obd_pair_tick(obd_pair_t *p, int64_t now_ms);      /* the ask time ran out: EXPIRED */
void        obd_pair_user_search(obd_pair_t *p);                     /* the user's Find OBD: expired devices may come back */
/* the open question for the screen: false when none. Any out pointer may be NULL. */
bool        obd_pair_offer(const obd_pair_t *p, int64_t now_ms, char *name, size_t n, const char **strength, int *secs_left);
const char *obd_pair_strength(int rssi);                             /* "strong" or "good" */
