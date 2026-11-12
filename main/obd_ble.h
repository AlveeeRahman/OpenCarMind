#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"
#if CONFIG_OBD_BLE_PAIR_CONFIRM
#include "board.h"                 /* BOARD_MOSAICO: the pairing safety below */
#endif

esp_err_t obd_ble_start(void);
bool      obd_ble_connected(void);
/* Pause scanning (an existing link is never dropped); while paused, obd_ble_probe(ms) scans once for ms. */
void      obd_ble_set_paused(bool paused);
void      obd_ble_probe(uint32_t ms);
/* Erase the saved adapter address and go back to name/service discovery. */
void      obd_ble_forget_adapter(void);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
/* Called by elm327.c once the device answered the ELM327 reset. True: it is the paired adapter, go on. False: a new
   adapter (the link is being dropped and the user asked, obd_ble_pair_*) or no link: send nothing more. */
bool      obd_ble_adapter_confirmed(void);
#else
/* Called by elm327.c once the device answered the ELM327 reset: keeps its address for later boots. */
void      obd_ble_adapter_confirmed(void);
#endif
/* Called by elm327.c when the device never answered like an ELM327: drops it, and skips it for a while if unsaved. */
void      obd_ble_adapter_rejected(const char *why);
/* Transport write used by elm327.c; splits to the ATT MTU; returns 0 on success. */
int       obd_ble_write(const uint8_t *data, size_t len);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
/* Connection search. user = true (Find OBD, the BOOT double-click): clears the failure skip list and the unanswered
   questions and, for 90 s, lets a different adapter be offered next to the saved one. user = false (the automatic search):
   changes nothing, so it never replaces or bypasses a saved adapter. A new device must look like an OBD adapter and have a
   strong signal (CONFIG_OBD_BLE_PAIR_MIN_RSSI), and is saved only after the user's yes. */
void      obd_ble_explore(bool user);
/* The pairing question (obd_pair.h). offer: true while one is open (name, "strong"/"good", seconds left; any may be NULL).
   seq: +1 for every new question. answer: the user's yes ("Use this adapter") or no ("Not this one"); a late answer is
   ignored. poll: once a second, closes a question whose time ran out (not saved). answer and poll may write NVS: call them
   from a task with an internal-RAM stack. */
bool      obd_ble_pair_offer(char *name, size_t n, const char **strength, int *secs_left);
uint32_t  obd_ble_pair_seq(void);
void      obd_ble_pair_answer(bool yes);
void      obd_ble_pair_poll(void);
#endif
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
/* Pairing safety on the ESP-Mosaico (adapter_guard.h: R1 the adapter fingerprint, R2 no silent re-pair).
   elm: elm327.c hands over the paired adapter's ATI and AT@1 replies; the fingerprint is checked (or learned on the first
   link after the yes) and the verdict logged. May write NVS: the obd task (internal-RAM stack).
   protocol: the ATDPN reply, a soft field (noted, never a mismatch). changed: this link's adapter looks different or its
   re-pair was refused, so its rows are marked and the learner gets none of them. meta: the obd task, every row: writes the
   session's "adapter_check" when it changed. offer/answer: the guard question for the screen (one at a time, the pairing
   sheet's style); seq tells a late answer apart; answer may write NVS (the board task). */
void      obd_ble_guard_elm(const char *ati, const char *at1);
void      obd_ble_guard_protocol(const char *dpn);
bool      obd_ble_adapter_changed(void);
void      obd_ble_guard_meta(void);
bool      obd_ble_guard_offer(const char **text, const char **yes, const char **no, const char **note, int *secs_left,
                              uint32_t *seq);
void      obd_ble_guard_answer(uint32_t seq, bool yes);
void      obd_ble_guard_counts(uint32_t *fp_checks, uint32_t *repair_refused);   /* the Health app (plain reads) */
#else
#define obd_ble_guard_offer(text, yes, no, note, secs_left, seq) \
    ((void)(text), (void)(yes), (void)(no), (void)(note), (void)(secs_left), (void)(seq), false)
#define obd_ble_guard_answer(seq, yes) ((void)(seq), (void)(yes))
#endif
#if !CONFIG_OBD_BLE_PAIR_CONFIRM
/* Connection search: clears the skip list and, for 90 s, lifts the saved-adapter lock so a different adapter can be found.
   A device must still look like an OBD adapter (name or serial service): other people's devices are never tried. */
void      obd_ble_explore(void);
/* Pairing confirm off: no question is ever open (the ESP-Mosaico's sheet stays closed). */
#define obd_ble_pair_offer(name, n, strength, secs_left) ((void)(name), (void)(n), (void)(strength), (void)(secs_left), false)
#define obd_ble_pair_seq()       0u
#define obd_ble_pair_answer(yes) ((void)(yes))
#define obd_ble_pair_poll()      ((void)0)
#endif
