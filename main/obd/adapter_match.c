#include "adapter_match.h"
#include <ctype.h>
#include <string.h>

static bool starts_with_nocase(const char *name, size_t len, const char *prefix, size_t plen)
{
    if (plen == 0 || len < plen) return false;
    for (size_t i = 0; i < plen; i++)
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)prefix[i])) return false;
    return true;
}

bool adapter_name_matches(const char *name, size_t len, const char *prefixes)
{
    if (!name || !prefixes) return false;
    const char *p = prefixes;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *start = p;
        while (n && isspace((unsigned char)*start)) { start++; n--; }
        while (n && isspace((unsigned char)start[n - 1])) n--;
        if (starts_with_nocase(name, len, start, n)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

/* Words seen in the advertised names of OBD-II adapters, collected from BetterDriver, SwiftOBD2, ha-obd-ble,
   EdiabasLib and vendor listings. */
static const char *const OBD_WORDS[] = {
    "obd", "elm327", "elm 327", "vlink", "v-link", "vgate", "icar", "konnwei", "kw90", "viecar", "veepeak",
    "carista", "kiwi", "lelink", "unicarscan", "tonwon", "carly", "autophix", "bafx", "ancel", "fixd",
};

bool adapter_name_looks_obd(const char *name, size_t len)
{
    if (!name) return false;
    for (size_t w = 0; w < sizeof OBD_WORDS / sizeof *OBD_WORDS; w++) {
        size_t wl = strlen(OBD_WORDS[w]);
        for (size_t i = 0; i + wl <= len; i++)
            if (starts_with_nocase(name + i, len - i, OBD_WORDS[w], wl)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ UUIDs */
static const uint8_t BASE[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

adapter_uuid_t adapter_uuid16(uint16_t v)
{
    adapter_uuid_t u;
    memcpy(u.b, BASE, 16);
    u.b[2] = (uint8_t)(v >> 8);
    u.b[3] = (uint8_t)v;
    return u;
}

bool adapter_uuid_parse(const char *s, adapter_uuid_t *out)
{
    if (!s || !out) return false;
    uint8_t nib[32];
    size_t n = 0;
    for (; *s; s++) {
        if (*s == '-' || *s == ' ') continue;
        int v = hexv(*s);
        if (v < 0 || n >= 32) return false;
        nib[n++] = (uint8_t)v;
    }
    if (n != 4 && n != 8 && n != 32) return false;
    memcpy(out->b, BASE, 16);
    size_t off = n == 4 ? 4 : 0;           /* a 16-bit UUID sits in bytes 2..3 of the base */
    for (size_t i = 0; i < n; i += 2) out->b[(off + i) / 2] = (uint8_t)(nib[i] << 4 | nib[i + 1]);
    return true;
}

bool adapter_uuid_eq(const adapter_uuid_t *a, const adapter_uuid_t *b) { return memcmp(a->b, b->b, 16) == 0; }

static bool is_base16(const adapter_uuid_t *u, uint16_t *v)
{
    if (u->b[0] || u->b[1] || memcmp(u->b + 4, BASE + 4, 12) != 0) return false;
    *v = (uint16_t)(u->b[2] << 8 | u->b[3]);
    return true;
}

/* SIG-assigned services (GAP, GATT, device information, battery, ...) never carry the serial line. 18F0 is not
   SIG-assigned and is used by Vgate, so the range stops below it. */
static bool is_standard_service(const adapter_uuid_t *u)
{
    uint16_t v;
    return is_base16(u, &v) && v >= 0x1800 && v < 0x18F0;
}

/* ------------------------------------------------------------------ profiles */
typedef struct { const char *name, *svc, *ntf, *wr; bool advertised; } profile_t;

/* Sources: FFF0 = Veepeak, Carista, OBDLink CX and most clones (Stack Overflow 61090365, obd-ble-serial);
   FFE0/FFE1 = LELink and HM-10 style (bench-tested); FFE0/FFE1/FFE2 = Deep OBD (EdiabasLib);
   E7810A71/BEF8D6C9 = Vgate vLinker (EdiabasLib, LTSupportAutomotive, ha-obd-ble);
   18F0/2AF0/2AF1 = Vgate iCar Pro BLE (SwiftOBD2, ABRP); BEEF = LTSupportAutomotive's list, characteristics
   picked by property; 6E400001 = Nordic UART (AndrOBD). */
static const profile_t PROFILES[] = {
    {"FFF0",              "FFF0", "FFF1", "FFF2", true},
    {"FFE0",              "FFE0", "FFE1", "FFE1", false},
    {"FFE0 split",        "FFE0", "FFE1", "FFE2", false},
    {"Vgate E7810A71",    "E7810A71-73AE-499D-8C15-FAA9AEF0C3F2", "BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F",
                          "BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F", true},
    {"Vgate 18F0",        "18F0", "2AF0", "2AF1", true},
    {"Nordic UART",       "6E400001-B5A3-F393-E0A9-E50E24DCCA9E", "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
                          "6E400002-B5A3-F393-E0A9-E50E24DCCA9E", false},
};
#define N_PROFILES (sizeof PROFILES / sizeof *PROFILES)

/* Services that are serial lines on OBD adapters but whose characteristics vary; matched only by advert. */
static const uint16_t EXTRA_ADVERTISED[] = {0xBEEF};

const char *adapter_profile_name(int profile)
{
    if (profile == -2) return "configured";
    if (profile < 0 || profile >= (int)N_PROFILES) return "generic";
    return PROFILES[profile].name;
}

bool adapter_service_is_obd(const adapter_uuid_t *u)
{
    for (size_t i = 0; i < N_PROFILES; i++) {
        adapter_uuid_t s;
        if (PROFILES[i].advertised && adapter_uuid_parse(PROFILES[i].svc, &s) && adapter_uuid_eq(&s, u)) return true;
    }
    for (size_t i = 0; i < sizeof EXTRA_ADVERTISED / sizeof *EXTRA_ADVERTISED; i++) {
        adapter_uuid_t s = adapter_uuid16(EXTRA_ADVERTISED[i]);
        if (adapter_uuid_eq(&s, u)) return true;
    }
    return false;
}

#define NOTIFIES(p) (((p) & (ADAPTER_PROP_NOTIFY | ADAPTER_PROP_INDICATE)) != 0)
#define WRITES(p)   (((p) & (ADAPTER_PROP_WRITE | ADAPTER_PROP_WRITE_NR)) != 0)

static bool try_profile(const adapter_chr_t *c, size_t n, const char *svc, const char *ntf, const char *wr,
                        int *ni, int *wi)
{
    adapter_uuid_t su, nu, wu;
    if (!svc || !svc[0] || !adapter_uuid_parse(svc, &su) || !adapter_uuid_parse(ntf, &nu) || !adapter_uuid_parse(wr, &wu))
        return false;
    int a = -1, b = -1;
    for (size_t i = 0; i < n; i++) {
        if (!adapter_uuid_eq(&c[i].svc, &su)) continue;
        if (a < 0 && adapter_uuid_eq(&c[i].chr, &nu) && NOTIFIES(c[i].props)) a = (int)i;
        if (b < 0 && adapter_uuid_eq(&c[i].chr, &wu) && WRITES(c[i].props)) b = (int)i;
    }
    if (a < 0 || b < 0) return false;
    *ni = a;
    *wi = b;
    return true;
}

bool adapter_choose(const adapter_chr_t *c, size_t n, const char *cfg_svc, const char *cfg_ntf, const char *cfg_wr,
                    int *notify_idx, int *write_idx, int *profile)
{
    if (!c || !notify_idx || !write_idx || !profile) return false;
    if (try_profile(c, n, cfg_svc, cfg_ntf, cfg_wr, notify_idx, write_idx)) { *profile = -2; return true; }
    for (size_t p = 0; p < N_PROFILES; p++)
        if (try_profile(c, n, PROFILES[p].svc, PROFILES[p].ntf, PROFILES[p].wr, notify_idx, write_idx)) {
            *profile = (int)p;
            return true;
        }

    /* Generic: services an OBD adapter advertises go first, then any other non-standard one, in discovery order. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < n; i++) {
            const adapter_uuid_t *svc = &c[i].svc;
            if (is_standard_service(svc) || (pass == 0) != adapter_service_is_obd(svc)) continue;
            bool seen = false;               /* handle each service once, at its first characteristic */
            for (size_t k = 0; k < i && !seen; k++) seen = adapter_uuid_eq(&c[k].svc, svc);
            if (seen) continue;
            int a = -1, b = -1, both = -1;
            for (size_t j = i; j < n; j++) {
                if (!adapter_uuid_eq(&c[j].svc, svc)) continue;
                if (a < 0 && NOTIFIES(c[j].props)) a = (int)j;
                if (b < 0 && WRITES(c[j].props)) b = (int)j;
                if (both < 0 && NOTIFIES(c[j].props) && WRITES(c[j].props)) both = (int)j;
            }
            if (a < 0 || b < 0) continue;
            if (both >= 0 && both == a) b = both;   /* one characteristic that does both is the line */
            *notify_idx = a;
            *write_idx = b;
            *profile = -1;
            return true;
        }
    }
    return false;
}
