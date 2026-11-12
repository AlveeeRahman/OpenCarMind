/* Pure rules for choosing and pairing a Bluetooth OBD adapter, see obd_pair.h. */
#include <string.h>
#include <stdio.h>
#include "obd_pair.h"

void obd_pair_init(obd_pair_t *p) { memset(p, 0, sizeof *p); }

static int skip_find(const obd_pair_t *p, const uint8_t *key)
{
    for (int i = 0; i < OBD_PAIR_NSKIP; i++)
        if (p->skip[i].why && memcmp(p->skip[i].key, key, OBD_PAIR_KEY_LEN) == 0) return i;
    return -1;
}

static void skip_add(obd_pair_t *p, const uint8_t *key, uint8_t why)
{
    int i = skip_find(p, key);
    if (i < 0) {
        i = p->skip_next;
        p->skip_next = (uint8_t)((p->skip_next + 1) % OBD_PAIR_NSKIP);
    }
    memcpy(p->skip[i].key, key, OBD_PAIR_KEY_LEN);
    p->skip[i].why = why;
}

bool obd_pair_skipped(const obd_pair_t *p, const uint8_t *key) { return skip_find(p, key) >= 0; }

obd_pick_t obd_pair_pick(const obd_pair_t *p, const obd_adv_t *a, int link_min_dbm, int pair_min_dbm)
{
    if (a->rssi < link_min_dbm) return OBD_PICK_NONE;
    if (a->have_saved && a->is_saved) return OBD_PICK_SAVED;
    if (a->have_saved && !a->user_search) return OBD_PICK_NONE;   /* only the user's Find OBD looks past a saved adapter */
    if (p->phase != OBD_PAIR_IDLE) return OBD_PICK_NONE;          /* one question at a time */
    if (!a->looks_obd || a->failed) return OBD_PICK_NONE;
    if (a->rssi < pair_min_dbm) return OBD_PICK_NONE;              /* a first pairing needs a strong signal */
    if (obd_pair_skipped(p, a->key)) return OBD_PICK_NONE;
    return OBD_PICK_OFFER;
}

void obd_pair_probe(obd_pair_t *p, const uint8_t *key, const char *name, int rssi)
{
    p->phase = OBD_PAIR_PROBING;
    memcpy(p->key, key, OBD_PAIR_KEY_LEN);
    size_t k = 0;
    for (; name && *name && k + 1 < sizeof p->name; name++)
        if (*name >= 0x20 && *name < 0x7F) p->name[k++] = *name;
    p->name[k] = 0;
    p->rssi = rssi;
    p->until_ms = 0;
}

bool obd_pair_is_probing(const obd_pair_t *p, const uint8_t *key)
{
    return p->phase == OBD_PAIR_PROBING && memcmp(p->key, key, OBD_PAIR_KEY_LEN) == 0;
}

void obd_pair_probe_failed(obd_pair_t *p)
{
    if (p->phase == OBD_PAIR_PROBING) p->phase = OBD_PAIR_IDLE;
}

bool obd_pair_probe_answered(obd_pair_t *p, int64_t now_ms, uint32_t ask_ms)
{
    if (p->phase != OBD_PAIR_PROBING) return false;
    p->phase = OBD_PAIR_ASKING;
    p->until_ms = now_ms + ask_ms;
    p->seq++;
    return true;
}

static obd_pair_result_t finish(obd_pair_t *p, obd_done_t d)
{
    obd_pair_result_t r = {.done = d};
    memcpy(r.key, p->key, OBD_PAIR_KEY_LEN);
    memcpy(r.name, p->name, sizeof r.name);
    if (d == OBD_DONE_DECLINED) skip_add(p, p->key, OBD_SKIP_DECLINED);
    if (d == OBD_DONE_EXPIRED) skip_add(p, p->key, OBD_SKIP_EXPIRED);
    p->phase = OBD_PAIR_IDLE;
    p->until_ms = 0;
    return r;
}

obd_pair_result_t obd_pair_answer(obd_pair_t *p, obd_answer_t a, int64_t now_ms)
{
    obd_pair_result_t none = {.done = OBD_DONE_NONE};
    if (p->phase != OBD_PAIR_ASKING) return none;                 /* a late or repeated tap */
    if (now_ms >= p->until_ms) return finish(p, OBD_DONE_EXPIRED); /* the time was up before the answer arrived */
    return finish(p, a == OBD_ANS_YES ? OBD_DONE_SAVE : OBD_DONE_DECLINED);
}

obd_pair_result_t obd_pair_tick(obd_pair_t *p, int64_t now_ms)
{
    obd_pair_result_t none = {.done = OBD_DONE_NONE};
    if (p->phase != OBD_PAIR_ASKING || now_ms < p->until_ms) return none;
    return finish(p, OBD_DONE_EXPIRED);
}

void obd_pair_user_search(obd_pair_t *p)
{
    for (int i = 0; i < OBD_PAIR_NSKIP; i++)
        if (p->skip[i].why == OBD_SKIP_EXPIRED) p->skip[i].why = 0;   /* "Not this one" stays skipped for this boot */
}

const char *obd_pair_strength(int rssi) { return rssi >= OBD_PAIR_STRONG_DBM ? "strong" : "good"; }

bool obd_pair_offer(const obd_pair_t *p, int64_t now_ms, char *name, size_t n, const char **strength, int *secs_left)
{
    if (p->phase != OBD_PAIR_ASKING || now_ms >= p->until_ms) return false;
    if (name && n) snprintf(name, n, "%s", p->name[0] ? p->name : "OBD adapter");
    if (strength) *strength = obd_pair_strength(p->rssi);
    if (secs_left) *secs_left = (int)((p->until_ms - now_ms + 999) / 1000);
    return true;
}
