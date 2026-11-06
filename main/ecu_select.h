/* Which computer to poll when several answer, and the connection-search summary. Pure C, no ESP-IDF, covered by
 * tests/host. On CAN, more than one computer can answer a broadcast request (engine and transmission, hybrid systems), in
 * either order; reading the first reply then gives the wrong supported-PID list. These helpers parse the replies to
 * 0100 sent with headers on and pick the engine, so the logger can address it directly (ISO 15765-4: 11-bit request id
 * = reply id - 8; 29-bit 18DAxxF1). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char    resp_id[9];   /* "7E8" or "18DAF110" */
    char    req_hdr[9];   /* physical request header: "7E0" or "18DA10F1" */
    uint8_t sup01[4];     /* the computer's 0100 bitmask (PIDs 01-20) */
} ecu_t;

/* Parses the reply to a broadcast 0100 sent with ATH1 ATS0 on CAN: one line per computer, 11-bit "7E8064100BE3EA813"
   (id, length byte, data) or 29-bit "18DAF110064100BE3EA813". Ids outside 7E8..7EF / 18DAF1xx are skipped. Sorted by id,
   no duplicates. Returns the number written to out. */
int ecu_parse_replies(const char *resp, bool ext29, ecu_t *out, int max);

/* The engine among them: 7E8 (11-bit) or 18DAF110 (29-bit) whenever it answers, the usual engine ids across Japanese and
   US makes; otherwise the computer that lists RPM (PID 0C) with the most supported PIDs, lowest id on a tie. Listing RPM
   and speed alone is not enough: a transmission can list both. The caller confirms the pick with 010C on its address.
   Returns the index, or -1 when none qualifies. */
int ecu_pick_engine(const ecu_t *e, int n);

/* One line for the screen: protocol, each computer and whether it answered RPM on its own address, the address used.
   Example "A6: 7E8 rpm, 7E9 -; use 7E0". pick < 0 = none. Returns the length. */
size_t ecu_search_summary(const char *proto, const ecu_t *e, const bool *rpm_ok, int n, int pick, char *out, size_t cap);
