/* Host tests for the adapter choice in both states of CONFIG_OBD_BLE_PAIR_CONFIRM: is_candidate() of main/obd_ble.c,
   cut out of the firmware source with the switch off and on (unifdef + awk in the Makefile's pair_gate_tests rule) and
   run here against stand-ins for NimBLE and the clock. Off is the car board's tested rule (EdgeOS 2.0.0, 4aeff43); on is
   "ask + strong signal" (obd_pair.c). Build and run: make -C tests/host pair. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "adapter_match.h"
#include "obd_pair.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the firmware's defaults (main/Kconfig.projbuild) */
#define CONFIG_OBD_BLE_MIN_RSSI      (-90)
#define CONFIG_OBD_BLE_PAIR_MIN_RSSI (-70)
#define CONFIG_OBD_BLE_NAME_PREFIXES "OBDBLE,OBDII,OBD2,OBDLink,ELM327,VEEPEAK,vLinker,V-LINK,IOS-Vlink,Vgate,iCar,Viecar,KONNWEI"

/* NimBLE stand-ins: only what is_candidate() reads */
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
struct ble_gap_disc_desc { uint8_t event_type; int8_t rssi; ble_addr_t addr; };
struct ble_hs_adv_fields { const uint8_t *name; uint8_t name_len; bool serial; };
#define BLE_HCI_ADV_RPT_EVTYPE_ADV_IND     0
#define BLE_HCI_ADV_RPT_EVTYPE_DIR_IND     1
#define BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND 3
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP    4
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m)  ((void)(m))

static int ble_addr_cmp(const ble_addr_t *a, const ble_addr_t *b) { return memcmp(a, b, sizeof *a); }
static int64_t g_now_us;
static int64_t esp_timer_get_time(void) { return g_now_us; }
static bool g_rejected;
static bool is_rejected(const ble_addr_t *a) { (void)a; return g_rejected; }
static bool advertises_serial(const struct ble_hs_adv_fields *f) { return f->serial; }

/* obd_ble.c's state that is_candidate() reads */
static int64_t    s_explore_until;
static bool       s_have_saved;
static ble_addr_t s_saved_addr;
static int        s_pair_mux;
static obd_pair_t s_pair;

#include "pair_gate_off.inc"   /* static bool is_candidate_off(d, f): switch off */
#include "pair_gate_on.inc"    /* addr_key() and static obd_pick_t is_candidate_on(d, f): switch on */

static const ble_addr_t A_SAVED = {0, {3, 3, 3, 3, 3, 3}};
static const ble_addr_t A_OTHER = {0, {1, 1, 1, 1, 1, 1}};

static void reset(bool have_saved, bool exploring)
{
    g_now_us = 1000000;
    g_rejected = false;
    s_have_saved = have_saved;
    s_saved_addr = A_SAVED;
    s_explore_until = exploring ? g_now_us + 90LL * 1000000 : 0;
    obd_pair_init(&s_pair);
}

static struct ble_gap_disc_desc desc(const ble_addr_t *a, int rssi, uint8_t type)
{
    return (struct ble_gap_disc_desc){.event_type = type, .rssi = (int8_t)rssi, .addr = *a};
}

static struct ble_hs_adv_fields named(const char *name, bool serial)
{
    return (struct ble_hs_adv_fields){.name = (const uint8_t *)name, .name_len = (uint8_t)(name ? strlen(name) : 0),
                                      .serial = serial};
}

/* switch off: the car board's rule, unchanged since 4aeff43 */
static void test_gate_off(void)
{
    struct ble_hs_adv_fields obd = named("OBDBLE", false), phone = named("Pixel 8", false), serial = named(NULL, true);
    struct ble_gap_disc_desc d;

    reset(false, false);
    d = desc(&A_OTHER, -90, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_off(&d, &obd), "off, nothing saved: an OBD-named device at -90 dBm is a candidate");
    d = desc(&A_OTHER, -80, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_off(&d, &obd), "off, nothing saved: the bench's neighbour at -80 dBm is a candidate (old rule)");
    d = desc(&A_OTHER, -91, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(!is_candidate_off(&d, &obd), "off: below CONFIG_OBD_BLE_MIN_RSSI is never a candidate");
    d = desc(&A_OTHER, -50, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(!is_candidate_off(&d, &phone), "off: a device that does not look like an adapter is not a candidate");
    CHECK(is_candidate_off(&d, &serial), "off: an unnamed device with the serial service is a candidate");
    d = desc(&A_OTHER, -50, BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND);
    CHECK(!is_candidate_off(&d, &obd), "off: a non-connectable advert is not a candidate");
    d = desc(&A_OTHER, -50, BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP);
    CHECK(is_candidate_off(&d, &obd), "off: a scan response counts");
    g_rejected = true;
    CHECK(!is_candidate_off(&d, &obd), "off: a skipped device is not a candidate");

    reset(true, false);
    d = desc(&A_SAVED, -90, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_off(&d, &phone), "off, saved: the saved address is a candidate whatever its name");
    d = desc(&A_OTHER, -40, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(!is_candidate_off(&d, &obd), "off, saved, no search: another adapter is never a candidate");
    reset(true, true);
    d = desc(&A_OTHER, -85, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_off(&d, &obd), "off, saved, 90 s search: another adapter at -85 dBm is a candidate (old rule)");
    g_now_us = s_explore_until;
    CHECK(!is_candidate_off(&d, &obd), "off: the search window ends at s_explore_until");
}

/* switch on: "ask + strong signal" */
static void test_gate_on(void)
{
    struct ble_hs_adv_fields obd = named("OBDBLE", false), phone = named("Pixel 8", false);
    struct ble_gap_disc_desc d;

    reset(false, false);
    d = desc(&A_OTHER, -80, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_on(&d, &obd) == OBD_PICK_NONE, "on, nothing saved: the neighbour at -80 dBm is not offered");
    d = desc(&A_OTHER, -70, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_on(&d, &obd) == OBD_PICK_OFFER, "on, nothing saved: an adapter at -70 dBm is offered");
    d = desc(&A_OTHER, -50, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_on(&d, &phone) == OBD_PICK_NONE, "on: a device that does not look like an adapter is not offered");
    d = desc(&A_OTHER, -50, BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND);
    CHECK(is_candidate_on(&d, &obd) == OBD_PICK_NONE, "on: a non-connectable advert is not offered");

    reset(true, false);
    d = desc(&A_SAVED, -90, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_on(&d, &phone) == OBD_PICK_SAVED, "on, saved: the saved address connects down to -90 dBm");
    d = desc(&A_OTHER, -40, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_on(&d, &obd) == OBD_PICK_NONE, "on, saved, no user search: another adapter is never offered");
    reset(true, true);
    d = desc(&A_OTHER, -85, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_on(&d, &obd) == OBD_PICK_NONE, "on, Find OBD: another adapter at -85 dBm is not offered");
    d = desc(&A_OTHER, -65, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(is_candidate_on(&d, &obd) == OBD_PICK_OFFER, "on, Find OBD: another adapter at -65 dBm is offered");
}

int main(void)
{
    test_gate_off();
    test_gate_on();
    printf("pair gate (CONFIG_OBD_BLE_PAIR_CONFIRM off and on): %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
