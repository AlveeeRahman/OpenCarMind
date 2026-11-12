/* Host tests for the pairing safety rules (main/adapter_guard.c): the adapter fingerprint (R1), the re-pair table (R2)
   and the guard question. Build and run: make -C tests/host guard (also runs guard_audit.sh, the source rules). */
#include <stdio.h>
#include <string.h>
#include "adapter_guard.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char VIN[] = "WVWZZZ1KZAW123456";

/* the owner's adapter as the board sees it: LELink-style 16-bit services, a maker id with a changing battery byte */
typedef struct {
    uint8_t addr_type;
    bool scan_rsp;
    const char *name;
    uint16_t u16[4]; size_t n16;
    uint8_t u128[2][16]; size_t n128;
    uint8_t mfg[6]; size_t mfg_len;
    uint8_t svc[16], ntf[16], wr[16];
    const char *ati, *at1, *proto;
} link_t;

static link_t lelink(void)
{
    link_t l = {.addr_type = 0, .scan_rsp = false, .name = "OBDBLE", .u16 = {0xFFF0, 0x180A}, .n16 = 2, .n128 = 0,
                .mfg = {0x5D, 0x00, 0x01, 0x64}, .mfg_len = 4, .ati = "ELM327 v1.5", .at1 = "OBDII to RS232 Interpreter",
                .proto = "A6"};
    static const uint8_t base[16] = {0, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
    memcpy(l.svc, base, 16); l.svc[2] = 0xFF; l.svc[3] = 0xF0;
    memcpy(l.ntf, base, 16); l.ntf[2] = 0xFF; l.ntf[3] = 0xF1;
    memcpy(l.wr, base, 16);  l.wr[2] = 0xFF;  l.wr[3] = 0xF2;
    return l;
}

static adapter_fp_t fp_of(const link_t *l)
{
    adapter_fp_t fp;
    afp_init(&fp);
    afp_set_addr_type(&fp, l->addr_type);
    afp_adv_t a = {.name = (const uint8_t *)l->name, .name_len = l->name ? strlen(l->name) : 0, .uuid16 = l->u16,
                   .n16 = l->n16, .uuid128 = (const uint8_t (*)[16])l->u128, .n128 = l->n128, .mfg = l->mfg,
                   .mfg_len = l->mfg_len};
    afp_set_adv(&fp, l->scan_rsp, &a);
    afp_set_gatt(&fp, l->svc, l->ntf, l->wr);
    afp_set_elm(&fp, AFP_ATI, l->ati);
    afp_set_elm(&fp, AFP_AT1, l->at1);
    afp_set_proto(&fp, l->proto);
    return fp;
}

static bool same(const link_t *a, const link_t *b)
{
    adapter_fp_t x = fp_of(a), y = fp_of(b);
    return afp_same(&x, &y, NULL);
}

/* 1. benign variation keeps the fingerprint */
static void test_stable(void)
{
    link_t ref = lelink(), l;
    adapter_fp_t r = fp_of(&ref);
    CHECK(afp_valid(&r), "a full record is valid");
    CHECK(sizeof r == 48, "the NVS record is %zu bytes", sizeof r);

    l = ref; l.u16[0] = 0x180A; l.u16[1] = 0xFFF0;
    CHECK(same(&ref, &l), "service UUID order does not matter");
    l = ref; l.u16[2] = 0xFFF0; l.n16 = 3;
    CHECK(same(&ref, &l), "a repeated service UUID does not matter");
    l = ref; l.mfg[3] = 0x31;
    CHECK(same(&ref, &l), "manufacturer bytes after the company id (battery) do not matter");
    l = ref; l.name = "OBDBLE  ";
    CHECK(same(&ref, &l), "trailing spaces in the name do not matter");
    l = ref; l.ati = "elm327 V1.5\n"; l.at1 = "  OBDII to RS232 interpreter\r";
    CHECK(same(&ref, &l), "case, spaces and line ends in the ELM replies do not matter");
    l = ref; l.at1 = NULL;
    CHECK(same(&ref, &l), "no AT@1 reply (a clone that never answers it) is unknown, not a mismatch");
    l = ref; l.proto = "A7";
    {
        adapter_fp_t x = fp_of(&ref), y = fp_of(&l);
        uint16_t d;
        CHECK(afp_same(&x, &y, &d) && d == (1u << AFP_PROTO), "another car's protocol is noted (diff %04x), not a mismatch", d);
    }
    l = ref; l.proto = "6";
    CHECK(same(&ref, &l), "ATDPN 6 and A6 are the same protocol");
    l = ref; l.proto = "0";
    CHECK(same(&ref, &l), "no protocol yet (0) is unknown");
    /* adverts and scan responses: the board connects on the first packet it sees */
    l = ref; l.scan_rsp = true; l.n16 = 0; l.mfg_len = 0; l.name = "OBDBLE Pro";
    CHECK(same(&ref, &l), "a scan response is compared only with a scan response (the record has none)");
    l = ref; l.name = NULL;
    CHECK(same(&ref, &l), "a packet without the name is unknown, not a mismatch");
    l = ref; l.n16 = 0;
    CHECK(same(&ref, &l), "a packet without services is unknown, not a mismatch");
    l = ref; l.mfg_len = 0;
    CHECK(same(&ref, &l), "a packet without manufacturer data is unknown, not a mismatch");
    /* the same adapter twice gives the same id; the protocol (soft) is not part of it */
    l = ref; l.proto = "A8";
    adapter_fp_t a = fp_of(&ref), b = fp_of(&l);
    CHECK(afp_id(&a) == afp_id(&b), "the logged id leaves the soft protocol out");
}

/* 2. a clone that copies only the address (and maybe the name) is caught */
static void test_clone(void)
{
    link_t ref = lelink(), l;
    l = ref; l.name = "OBDII";
    CHECK(!same(&ref, &l), "another name in the same packet kind is a mismatch");
    l = ref; l.u16[0] = 0xFFE0;
    CHECK(!same(&ref, &l), "another service set is a mismatch");
    l = ref; l.n128 = 1; memset(l.u128[0], 0x6E, 16);
    CHECK(!same(&ref, &l), "an extra 128-bit service is a mismatch");
    l = ref; l.mfg[0] = 0xE5; l.mfg[1] = 0x02;          /* Espressif's company id: an ESP32 posing as the adapter */
    CHECK(!same(&ref, &l), "another maker id is a mismatch");
    l = ref; l.ati = "ELM327 v2.1";
    CHECK(!same(&ref, &l), "another ATI reply is a mismatch");
    l = ref; l.at1 = "?";
    CHECK(!same(&ref, &l), "AT@1 answered '?' where the adapter gave a description: a mismatch");
    l = ref; l.ntf[3] = 0xE1; l.wr[3] = 0xE1; l.svc[3] = 0xE0;
    CHECK(!same(&ref, &l), "another serial line (FFE0/FFE1) is a mismatch");
    l = ref; l.addr_type = 1;
    CHECK(!same(&ref, &l), "a random address where the adapter has a public one is a mismatch");
    {
        adapter_fp_t r = fp_of(&ref), n = fp_of(&ref);
        n.known &= (uint16_t)~(1u << AFP_ATI);           /* the copy stays silent on ATI */
        uint16_t d;
        CHECK(!afp_same(&r, &n, &d) && (d & (1u << AFP_ATI)), "a core field the record has but the link did not give is a mismatch");
        n = fp_of(&ref);
        n.known &= (uint16_t)~(1u << AFP_GATT);
        CHECK(!afp_same(&r, &n, NULL), "no serial line in the record of this link: a mismatch");
    }
    l = ref; l.name = "OBDII"; l.ati = "ELM327 v2.1";
    adapter_fp_t a = fp_of(&ref), b = fp_of(&l);
    CHECK(afp_id(&a) != afp_id(&b), "a clone gets another id");
    uint16_t d;
    afp_same(&a, &b, &d);
    CHECK(afp_first(d & ~AFP_SOFT) == AFP_ADV_NAME && !strcmp(afp_field_name(AFP_ADV_NAME), "name"),
          "the event names the first differing field");
}

/* 3. the VIN never reaches the record or the logged text */
static bool contains(const void *hay, size_t n, const char *needle, size_t k)
{
    const uint8_t *h = hay;
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(h + i, needle, k)) return true;
    return false;
}

static void test_no_vin(void)
{
    link_t ref = lelink();
    /* even an adapter that answers ATI or AT@1 with the VIN (a clone that echoes the last reply) only gives a hash */
    ref.ati = "ELM327 v1.5 WVWZZZ1KZAW123456";
    ref.at1 = VIN;
    adapter_fp_t fp = fp_of(&ref);
    bool leak = false;
    for (size_t k = 4; k <= strlen(VIN); k++)
        for (size_t i = 0; i + k <= strlen(VIN); i++) leak |= contains(&fp, sizeof fp, VIN + i, k);
    CHECK(!leak, "no 4-character piece of the VIN is in the stored record");
    /* every text the guard logs: the field names, the questions and the formats of obd_ble.c's lines */
    char line[256];
    bool in_text = false;
    for (int f = 0; f < AFP_N; f++) in_text |= strstr(afp_field_name((afp_field_t)f), "WVW") != NULL;
    for (int k = AG_Q_NONE; k <= AG_Q_REPAIR; k++) in_text |= strstr(ag_q_text((ag_q_kind_t)k), "WVW") != NULL;
    snprintf(line, sizeof line, "Adapter fingerprint saved (%08lx)", (unsigned long)afp_id(&fp));
    in_text |= strstr(line, VIN) != NULL;
    snprintf(line, sizeof line, "{\"result\":\"%s\",\"fp\":\"%08lx\",\"repair\":\"%s\",\"repair_events\":%u,\"changed_links\":%u}",
             "changed", (unsigned long)afp_id(&fp), "none", 0u, 1u);
    in_text |= strstr(line, VIN) != NULL || strlen(line) > 159;
    CHECK(!in_text, "no logged text carries the VIN, and the session line fits its buffer");
}

/* 4. the R1 table */
static void test_r1(void)
{
    link_t ref = lelink(), clone = lelink();
    clone.ati = "ELM327 v2.1";
    adapter_fp_t r = fp_of(&ref), c = fp_of(&clone), bad = r;
    bad.version = 9;
    uint16_t d;
    CHECK(ag_r1_decide(NULL, &r, false, &d) == AG_R1_LEARN, "no record: learn (first link after the yes)");
    CHECK(ag_r1_decide(NULL, &r, true, &d) == AG_R1_LEARN, "no record while driving: learn too (paired before this check)");
    CHECK(ag_r1_decide(&bad, &r, false, &d) == AG_R1_LEARN, "a record of another version: learn again");
    CHECK(ag_r1_decide(&r, &r, false, &d) == AG_R1_USE && d == 0, "same, parked: use");
    CHECK(ag_r1_decide(&r, &r, true, &d) == AG_R1_USE, "same, driving: use");
    CHECK(ag_r1_decide(&r, &c, false, &d) == AG_R1_ASK && (d & (1u << AFP_ATI)), "different, parked: ask");
    CHECK(ag_r1_decide(&r, &c, true, &d) == AG_R1_FLAG, "different, driving: flag only, never cut the link");
}

/* 5. the R2 table: driving x allowed x another question open */
static void test_r2(void)
{
    static const struct { bool drv, allow, busy; ag_r2_t want; const char *why; } T[] = {
        {false, false, false, AG_R2_ASK,    "parked, not allowed: ask"},
        {false, false, true,  AG_R2_REFUSE, "parked, another question open: refuse (one question at a time)"},
        {false, true,  false, AG_R2_ALLOW,  "parked, the user said Allow: pair again"},
        {false, true,  true,  AG_R2_ALLOW,  "parked, allowed, a question open: the Allow still holds"},
        {true,  false, false, AG_R2_REFUSE, "driving: refuse and log"},
        {true,  false, true,  AG_R2_REFUSE, "driving, a question open: refuse"},
        {true,  true,  false, AG_R2_ALLOW,  "driving within the Allow window: pair again"},
        {true,  true,  true,  AG_R2_ALLOW,  "driving, allowed, busy: pair again"},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        CHECK(ag_r2_decide(T[i].drv, T[i].allow, T[i].busy) == T[i].want, "%s", T[i].why);
}

/* 6. the guard question */
static void test_question(void)
{
    ag_q_t q;
    ag_q_init(&q);
    CHECK(!ag_q_offer(&q, 0, NULL, NULL, NULL), "nothing open at start");
    CHECK(ag_q_open(&q, AG_Q_CHANGED, 1000, 30000), "a question opens");
    CHECK(!ag_q_open(&q, AG_Q_REPAIR, 1000, 30000), "one question at a time");
    ag_q_kind_t k; int left; uint32_t seq;
    CHECK(ag_q_offer(&q, 1000, &k, &left, &seq) && k == AG_Q_CHANGED && left == 30, "the screen sees it, 30 s left (%d)", left);
    CHECK(!strcmp(ag_q_text(k), "This adapter looks different. Use it?") && !strcmp(ag_q_yes(k), "Use it") &&
          !strcmp(ag_q_no(k), "Not this one"), "the R1 question and buttons");
    CHECK(ag_q_answer(&q, seq + 1, 2000) == AG_Q_NONE, "an answer to another question is ignored");
    CHECK(ag_q_answer(&q, seq, 31000) == AG_Q_NONE, "a late answer is ignored");
    CHECK(ag_q_tick(&q, 30999) == AG_Q_NONE && ag_q_tick(&q, 31000) == AG_Q_CHANGED, "it expires at the ask time");
    CHECK(!ag_q_offer(&q, 31000, NULL, NULL, NULL), "closed after expiry");
    CHECK(ag_q_open(&q, AG_Q_REPAIR, 40000, 30000) && ag_q_offer(&q, 40000, &k, NULL, &seq) && k == AG_Q_REPAIR,
          "the next question opens");
    CHECK(!strcmp(ag_q_text(k), "The adapter asks to pair again. Allow?") && !strcmp(ag_q_yes(k), "Allow"), "the R2 question");
    CHECK(ag_q_answer(&q, seq, 41000) == AG_Q_REPAIR, "answered in time");
    CHECK(ag_q_answer(&q, seq, 41000) == AG_Q_NONE, "a second answer is ignored");
    ag_q_open(&q, AG_Q_CHANGED, 50000, 30000);
    ag_q_close(&q);
    CHECK(!ag_q_offer(&q, 50000, NULL, NULL, NULL) && ag_q_tick(&q, 90000) == AG_Q_NONE, "closed for a drive: no expiry action");
}

int main(void)
{
    test_stable();
    test_clone();
    test_no_vin();
    test_r1();
    test_r2();
    test_question();
    printf("adapter_guard: %d checks, %d failures\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
