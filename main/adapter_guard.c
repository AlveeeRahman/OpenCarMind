/* adapter_guard: the pure pairing-safety rules (adapter_guard.h). */
#include "adapter_guard.h"
#include <string.h>

/* the Bluetooth base UUID 00000000-0000-1000-8000-00805F9B34FB, most significant byte first */
static const uint8_t BASE[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};

uint32_t afp_fnv1a(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static void put(adapter_fp_t *fp, afp_field_t f, uint32_t h)
{
    fp->h[f] = h;
    fp->known |= (uint16_t)(1u << f);
}

void afp_init(adapter_fp_t *fp)
{
    memset(fp, 0, sizeof *fp);
    fp->version = AFP_VERSION;
}

void afp_set_addr_type(adapter_fp_t *fp, uint8_t type)
{
    put(fp, AFP_ADDR_TYPE, afp_fnv1a(AFP_FNV_INIT, &type, 1));
}

static int uuid_cmp(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 16); }

void afp_set_adv(adapter_fp_t *fp, bool scan_rsp, const afp_adv_t *a)
{
    afp_field_t fn = scan_rsp ? AFP_RSP_NAME : AFP_ADV_NAME, fu = scan_rsp ? AFP_RSP_UUIDS : AFP_ADV_UUIDS,
                fm = scan_rsp ? AFP_RSP_MFG : AFP_ADV_MFG;
    /* name: printable bytes only, trailing spaces and NULs dropped */
    if (a->name && a->name_len) {
        size_t n = a->name_len;
        while (n && (a->name[n - 1] == ' ' || a->name[n - 1] == 0)) n--;
        uint32_t h = AFP_FNV_INIT;
        size_t kept = 0;
        for (size_t i = 0; i < n; i++)
            if (a->name[i] >= 0x20 && a->name[i] < 0x7F) { h = afp_fnv1a(h, &a->name[i], 1); kept++; }
        if (kept) put(fp, fn, h);
    }
    /* service UUIDs: every one as 128 bits, most significant byte first, then sorted and without repeats */
    uint8_t u[AFP_MAX_UUIDS][16];
    size_t n = 0;
    for (size_t i = 0; i < a->n16 && n < AFP_MAX_UUIDS; i++, n++) {
        memcpy(u[n], BASE, 16);
        u[n][2] = (uint8_t)(a->uuid16[i] >> 8);
        u[n][3] = (uint8_t)a->uuid16[i];
    }
    for (size_t i = 0; i < a->n32 && n < AFP_MAX_UUIDS; i++, n++) {
        memcpy(u[n], BASE, 16);
        for (int k = 0; k < 4; k++) u[n][k] = (uint8_t)(a->uuid32[i] >> (24 - 8 * k));
    }
    for (size_t i = 0; i < a->n128 && n < AFP_MAX_UUIDS; i++, n++)
        for (int k = 0; k < 16; k++) u[n][k] = a->uuid128[i][15 - k];
    for (size_t i = 1; i < n; i++)                                   /* insertion sort: at most AFP_MAX_UUIDS */
        for (size_t j = i; j > 0 && uuid_cmp(u[j - 1], u[j]) > 0; j--) {
            uint8_t t[16];
            memcpy(t, u[j], 16); memcpy(u[j], u[j - 1], 16); memcpy(u[j - 1], t, 16);
        }
    if (n) {
        uint32_t h = AFP_FNV_INIT;
        for (size_t i = 0; i < n; i++)
            if (i == 0 || uuid_cmp(u[i - 1], u[i]) != 0) h = afp_fnv1a(h, u[i], 16);
        put(fp, fu, h);
    }
    /* manufacturer data: the company id only, the bytes after it may carry state */
    if (a->mfg && a->mfg_len >= 2) put(fp, fm, afp_fnv1a(AFP_FNV_INIT, a->mfg, 2));
}

void afp_set_gatt(adapter_fp_t *fp, const uint8_t svc[16], const uint8_t ntf[16], const uint8_t wr[16])
{
    uint32_t h = afp_fnv1a(AFP_FNV_INIT, svc, 16);
    h = afp_fnv1a(h, ntf, 16);
    put(fp, AFP_GATT, afp_fnv1a(h, wr, 16));
}

bool afp_set_elm(adapter_fp_t *fp, afp_field_t f, const char *reply)
{
    if ((f != AFP_ATI && f != AFP_AT1) || !reply) return false;
    uint32_t h = AFP_FNV_INIT;
    size_t kept = 0;
    for (const char *s = reply; *s; s++) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '?') { h = afp_fnv1a(h, &c, 1); kept++; }
    }
    if (!kept) return false;
    put(fp, f, h);
    return true;
}

bool afp_set_proto(adapter_fp_t *fp, const char *dpn)
{
    if (!dpn) return false;
    while (*dpn == ' ' || *dpn == '\n') dpn++;
    if (*dpn == 'A' || *dpn == 'a') dpn++;
    char c = *dpn;
    if (c >= 'a' && c <= 'c') c = (char)(c - 'a' + 'A');
    if (!((c >= '1' && c <= '9') || (c >= 'A' && c <= 'C'))) return false;   /* 0 = no protocol found yet */
    put(fp, AFP_PROTO, afp_fnv1a(AFP_FNV_INIT, &c, 1));
    return true;
}

bool afp_valid(const adapter_fp_t *fp)
{
    return fp && fp->version == AFP_VERSION && (fp->known & AFP_CORE) == AFP_CORE && !(fp->known >> AFP_N);
}

uint32_t afp_id(const adapter_fp_t *fp)
{
    uint32_t h = afp_fnv1a(AFP_FNV_INIT, &fp->known, sizeof fp->known);
    for (int f = 0; f < AFP_N; f++)
        if (fp->known & (1u << f) && !(AFP_SOFT & (1u << f))) h = afp_fnv1a(h, &fp->h[f], sizeof fp->h[f]);
    return h;
}

bool afp_same(const adapter_fp_t *stored, const adapter_fp_t *now, uint16_t *diff)
{
    uint16_t d = 0;
    bool same = true;
    for (int f = 0; f < AFP_N; f++) {
        uint16_t bit = (uint16_t)(1u << f);
        bool ks = stored->known & bit, kn = now->known & bit;
        if (ks && kn && stored->h[f] != now->h[f]) {
            d |= bit;
            if (!(AFP_SOFT & bit)) same = false;
        } else if (ks && !kn && (AFP_CORE & bit)) {
            d |= bit;
            same = false;
        }
    }
    if (diff) *diff = d;
    return same;
}

const char *afp_field_name(afp_field_t f)
{
    static const char *const N[AFP_N] = {"address type", "name", "services", "maker id", "name", "services", "maker id",
                                         "serial line", "ATI", "AT@1", "protocol"};
    return (unsigned)f < AFP_N ? N[f] : "?";
}

afp_field_t afp_first(uint16_t mask)
{
    for (int f = 0; f < AFP_N; f++) if (mask & (1u << f)) return (afp_field_t)f;
    return AFP_N;
}

ag_r1_t ag_r1_decide(const adapter_fp_t *stored, const adapter_fp_t *now, bool driving, uint16_t *diff)
{
    if (diff) *diff = 0;
    if (!stored || !afp_valid(stored)) return AG_R1_LEARN;
    if (afp_same(stored, now, diff)) return AG_R1_USE;
    return driving ? AG_R1_FLAG : AG_R1_ASK;
}

ag_r2_t ag_r2_decide(bool driving, bool user_allowed, bool question_open)
{
    if (user_allowed) return AG_R2_ALLOW;
    if (driving || question_open) return AG_R2_REFUSE;
    return AG_R2_ASK;
}

void ag_q_init(ag_q_t *q) { memset(q, 0, sizeof *q); }

bool ag_q_open(ag_q_t *q, ag_q_kind_t k, int64_t now_ms, uint32_t ask_ms)
{
    if (q->kind != AG_Q_NONE || k == AG_Q_NONE) return false;
    q->kind = k;
    q->until_ms = now_ms + ask_ms;
    q->seq++;
    return true;
}

ag_q_kind_t ag_q_answer(ag_q_t *q, uint32_t seq, int64_t now_ms)
{
    if (q->kind == AG_Q_NONE || seq != q->seq || now_ms >= q->until_ms) return AG_Q_NONE;
    ag_q_kind_t k = q->kind;
    q->kind = AG_Q_NONE;
    return k;
}

ag_q_kind_t ag_q_tick(ag_q_t *q, int64_t now_ms)
{
    if (q->kind == AG_Q_NONE || now_ms < q->until_ms) return AG_Q_NONE;
    ag_q_kind_t k = q->kind;
    q->kind = AG_Q_NONE;
    return k;
}

void ag_q_close(ag_q_t *q) { q->kind = AG_Q_NONE; }

bool ag_q_offer(const ag_q_t *q, int64_t now_ms, ag_q_kind_t *k, int *secs_left, uint32_t *seq)
{
    if (q->kind == AG_Q_NONE || now_ms >= q->until_ms) return false;
    if (k) *k = q->kind;
    if (secs_left) *secs_left = (int)((q->until_ms - now_ms + 999) / 1000);
    if (seq) *seq = q->seq;
    return true;
}

const char *ag_q_text(ag_q_kind_t k)
{
    return k == AG_Q_CHANGED ? "This adapter looks different. Use it?"
         : k == AG_Q_REPAIR  ? "The adapter asks to pair again. Allow?" : "";
}
const char *ag_q_yes(ag_q_kind_t k) { return k == AG_Q_CHANGED ? "Use it" : "Allow"; }
const char *ag_q_no(ag_q_kind_t k)  { return k == AG_Q_CHANGED ? "Not this one" : "Don't allow"; }
