/* adapter_guard: the pure rules that keep the saved OBD adapter honest on the ESP-Mosaico (pairing safety, R1 and R2 of
 * the net_ids design study). No IDF, no heap, no clock of its own: host-tested in tests/host/test_adapter_guard.c.
 * obd_ble.c builds the fingerprint, keeps the state under its lock and does what these rules return.
 *
 * R1, the adapter fingerprint. The saved adapter used to be known by its address alone, and a device that copies the
 * address could connect with no question. The fingerprint is one 32-bit FNV-1a hash per field below. Raw strings are
 * never kept: NVS holds the hashes, the log shows at most the 8-hex id (afp_id). The VIN is not a field (it names the car,
 * not the adapter, and must never be stored or logged), so no VIN can reach the record.
 *
 *   field        source                                         rule
 *   addr type    the advert (public or random)                  core: must match
 *   adv name     the advertised name in an advert (ADV_IND)     hard, compared only when both packets carried it
 *   adv uuids    the advertised service UUIDs, sorted, no dups  hard, as above
 *   adv mfg      the manufacturer data's company id (2 bytes)   hard, as above; the bytes after it are not used
 *   rsp name     the same three from a scan response            hard, compared only with a scan response's
 *   rsp uuids
 *   rsp mfg
 *   gatt         service, notify and write UUIDs of the serial  core: must match
 *                line adapter_choose() picked
 *   ati          the ATI reply, upper case, A-Z 0-9 . ? only    core: must match
 *   at@1         the AT@1 reply, same cleaning ("?" counts)     hard, skipped when the adapter gave no reply
 *   protocol     ATDPN, the leading "A" (auto) dropped          soft: a difference is noted, never a mismatch
 *
 * Tolerance, and why:
 *   - RSSI, the advert flags, TX power, the interval, service data and the manufacturer bytes after the company id are
 *     not used: they change from packet to packet or carry state (battery, counters).
 *   - Adverts and scan responses often carry different fields (the name only in the scan response, for example), and
 *     the board connects on the first packet it sees. So each kind is compared only with the same kind, and a field
 *     the packet did not carry is unknown, not a mismatch.
 *   - The order of the UUIDs in a packet does not matter (sorted set).
 *   - Upper/lower case, spaces and line ends in the ELM replies do not matter (cleaned before hashing).
 *   - AT@1: some clones never answer it; no reply is unknown. A "?" (not supported) is a stable answer and is compared.
 *   - The protocol is the car's, not the adapter's: moving the adapter to another car is a legitimate change.
 *   - A core field the saved record has but this link did not give (an ATI with no reply) counts as a mismatch, so a
 *     copy cannot pass by staying silent.
 *   - This is not cryptography: a device that copies every field passes. It stops the cheap clone that copies the address
 *     and the name. A hash is used for compact storage and so that no raw reply sits in NVS or the log.
 *
 * R1 decision (ag_r1_decide):
 *   no saved record          -> LEARN (the first link after the user's yes; also an adapter paired before this check)
 *   all compared fields same -> USE
 *   a mismatch, parked       -> ASK  "This adapter looks different. Use it?"; its rows are marked, the learner gets none
 *   a mismatch, driving      -> FLAG the link is never cut; rows marked, no learner; the question waits until parked
 *
 * R2, pairing again. NimBLE as a central (IDF 6.1) re-pairs by itself when the encryption with the saved bond fails with
 * "PIN or key missing" (BLE_RESTART_PAIR is 1 in esp_nimble_cfg.h, with no Kconfig), and the event
 * BLE_GAP_EVENT_REPEAT_PAIRING only comes for a pairing request, which a central never receives. obd_ble.c therefore
 * keeps a copy of the saved bond at connect and compares it after each encryption: a new key for the saved adapter
 * is a re-pair. The event handler uses the same table, in case another stack version sends it.
 *   user allowed it (Allow, within AG_ALLOW_MS) -> ALLOW  keep the new bond
 *   driving                                     -> REFUSE the old bond is put back, the link is kept but marked
 *   another question is open                    -> REFUSE as above
 *   parked                                      -> ASK    "The adapter asks to pair again. Allow?"; the link is held
 *                                                         (no subscribe, no data) until the answer
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AFP_ADDR_TYPE = 0,
    AFP_ADV_NAME, AFP_ADV_UUIDS, AFP_ADV_MFG,
    AFP_RSP_NAME, AFP_RSP_UUIDS, AFP_RSP_MFG,
    AFP_GATT, AFP_ATI, AFP_AT1, AFP_PROTO,
    AFP_N
} afp_field_t;

#define AFP_VERSION     1
#define AFP_CORE        ((1u << AFP_ADDR_TYPE) | (1u << AFP_GATT) | (1u << AFP_ATI))
#define AFP_SOFT        (1u << AFP_PROTO)
#define AFP_MAX_UUIDS   12      /* per packet; more are not hashed */

typedef struct {
    uint8_t  version;
    uint8_t  reserved;
    uint16_t known;             /* one bit per afp_field_t */
    uint32_t h[AFP_N];
} adapter_fp_t;

/* one advertising packet's stable parts; any pointer may be NULL with its count 0 */
typedef struct {
    const uint8_t  *name;  size_t name_len;
    const uint16_t *uuid16; size_t n16;
    const uint32_t *uuid32; size_t n32;
    const uint8_t (*uuid128)[16]; size_t n128;   /* little-endian, as in the packet */
    const uint8_t  *mfg;   size_t mfg_len;
} afp_adv_t;

uint32_t afp_fnv1a(uint32_t h, const void *p, size_t n);
#define  AFP_FNV_INIT 2166136261u

void     afp_init(adapter_fp_t *fp);
void     afp_set_addr_type(adapter_fp_t *fp, uint8_t type);
void     afp_set_adv(adapter_fp_t *fp, bool scan_rsp, const afp_adv_t *a);
void     afp_set_gatt(adapter_fp_t *fp, const uint8_t svc[16], const uint8_t ntf[16], const uint8_t wr[16]);
/* f = AFP_ATI or AFP_AT1; reply NULL or empty after cleaning: the field stays unknown (false) */
bool     afp_set_elm(adapter_fp_t *fp, afp_field_t f, const char *reply);
bool     afp_set_proto(adapter_fp_t *fp, const char *dpn);
bool     afp_valid(const adapter_fp_t *fp);       /* a record from NVS: right version and every core field known */
uint32_t afp_id(const adapter_fp_t *fp);          /* one hash over the known fields: the only form that is logged */
/* true when the records match under the rules above. diff (may be NULL): the fields that differ, soft ones included */
bool     afp_same(const adapter_fp_t *stored, const adapter_fp_t *now, uint16_t *diff);
const char *afp_field_name(afp_field_t f);        /* "name", "services", "ATI", ... for the event line */
afp_field_t afp_first(uint16_t mask);             /* lowest field in mask, AFP_N if none */

typedef enum { AG_R1_USE = 0, AG_R1_LEARN, AG_R1_ASK, AG_R1_FLAG } ag_r1_t;
ag_r1_t  ag_r1_decide(const adapter_fp_t *stored, const adapter_fp_t *now, bool driving, uint16_t *diff);

#define AG_ALLOW_MS  120000u    /* an "Allow" lets the adapter pair again within this time */
#define AG_HOLD_MS   600000u    /* "Not this one" or no answer: the saved address is not connected for 10 min */
typedef enum { AG_R2_ALLOW = 0, AG_R2_REFUSE, AG_R2_ASK } ag_r2_t;
ag_r2_t  ag_r2_decide(bool driving, bool user_allowed, bool question_open);

/* the one guard question at a time (the screen shows it in the pairing sheet's style) */
typedef enum { AG_Q_NONE = 0, AG_Q_CHANGED, AG_Q_REPAIR } ag_q_kind_t;
typedef struct { ag_q_kind_t kind; int64_t until_ms; uint32_t seq; } ag_q_t;
void        ag_q_init(ag_q_t *q);
bool        ag_q_open(ag_q_t *q, ag_q_kind_t k, int64_t now_ms, uint32_t ask_ms);   /* false: one is open already */
ag_q_kind_t ag_q_answer(ag_q_t *q, uint32_t seq, int64_t now_ms);    /* the kind answered; NONE when late or stale */
ag_q_kind_t ag_q_tick(ag_q_t *q, int64_t now_ms);                    /* the kind whose time ran out, else NONE */
void        ag_q_close(ag_q_t *q);
bool        ag_q_offer(const ag_q_t *q, int64_t now_ms, ag_q_kind_t *k, int *secs_left, uint32_t *seq);
const char *ag_q_text(ag_q_kind_t k);
const char *ag_q_yes(ag_q_kind_t k);
const char *ag_q_no(ag_q_kind_t k);
