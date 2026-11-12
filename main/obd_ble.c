/* NimBLE central for ELM327-style Bluetooth LE OBD-II adapters.
 *
 * CONFIG_OBD_BLE_PAIR_CONFIRM off (the car board's default, its tested rule): scan for adapters (by advertised name, by
 * advertised serial service, or by the configured prefixes), collect candidates for a short window, connect to the
 * strongest, find its serial line, and keep its address only after it answers the ELM327 reset. Devices that turn out not
 * to be adapters are skipped for a while. Later runs: connect only to the stored address. Long-press BOOT clears it.
 *
 * CONFIG_OBD_BLE_PAIR_CONFIRM on (the ESP-Mosaico's default; owner, 2026-11-11, "ask + strong signal"; the rules are
 * obd_pair.c): with no adapter saved, or in the user's Find OBD window, devices that look like adapters at
 * CONFIG_OBD_BLE_PAIR_MIN_RSSI or stronger are collected for a short window; the strongest is connected, its serial line
 * found and the ELM327 reset tried (the probe). If it answers, the link is dropped at once and the user is asked
 * (ESP-Mosaico: the pairing sheet; car board: tap BOOT within 5 s). Only a yes saves its address.
 * Later runs: connect only to the saved address (down to CONFIG_OBD_BLE_MIN_RSSI). The automatic search never looks past
 * it; only the user's Find OBD may offer another adapter, and switching needs the same yes. Forget clears it.
 * Devices that turn out not to be adapters are skipped for a while. The address is never shown or logged.
 * A bond made during a probe that was not confirmed is deleted.
 *
 * Pairing (Just Works) and bonding happen only when the adapter asks for them.
 */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_central.h"
#include "obd_ble.h"
#include "adapter_match.h"
#include "elm327.h"
#include "status.h"
#include "events.h"
#if CONFIG_OBD_BLE_PAIR_CONFIRM
#include "obd_pair.h"
#if BOARD_MOSAICO
#include "adapter_guard.h"
#include "esp_attr.h"
#include "sdlog.h"
#include "sys_mode.h"
#endif
#endif
#if BOARD_MOSAICO
#include "ids_svc.h"                   /* the network guard: hooks (counters only) and its verdict (OpenCarMind EdgeOS 2.1.0) */
#endif

static const char *TAG = "obd_ble";

void ble_store_config_init(void);

#define CANDIDATE_WINDOW_US  (1500 * 1000LL)   /* how long to collect candidates before picking the strongest */
#define REJECT_TTL_US        (10 * 60 * 1000000LL)
#define N_REJECT             8
#define MAX_CHRS             48
#if CONFIG_OBD_BLE_PAIR_CONFIRM
/* how long the question waits for an answer: the sheet on the ESP-Mosaico, a BOOT tap on the car board (owner: 30 s / 5 s) */
#if CONFIG_IDF_TARGET_ESP32S31
#define PAIR_ASK_MS          30000u
#else
#define PAIR_ASK_MS          5000u
#endif
#endif

static uint8_t   s_own_addr_type;
static uint16_t  s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t  s_write_handle, s_notify_handle, s_cccd_handle;
static bool      s_write_no_rsp, s_use_indicate;
static bool      s_ready;
static volatile int64_t s_explore_until;   /* esp_timer time until which the saved-adapter lock is lifted */
static bool      s_sec_tried;
static bool      s_pin_valid;          /* this address refused Just Works: pair it with the fixed PIN */
static ble_addr_t s_pin_addr;
static SemaphoreHandle_t s_write_done;

static ble_addr_t s_saved_addr;
static bool       s_have_saved;
#if CONFIG_OBD_BLE_PAIR_CONFIRM
static char       s_saved_name[32];
#endif
static char       s_cur_name[32];
static ble_addr_t s_cur_addr;

/* best candidate seen in the current scan window */
static bool       s_best_valid;
static ble_addr_t s_best_addr;
static char       s_best_name[32];
static int8_t     s_best_rssi;
static int64_t    s_window_start;
#if CONFIG_OBD_BLE_PAIR_CONFIRM

/* the pairing question (obd_pair.c): written by the NimBLE host task, the obd task, the button / board task, read by the
   screen; every access is a short critical section, the actions (NVS, bonds, links) run after it */
static portMUX_TYPE s_pair_mux = portMUX_INITIALIZER_UNLOCKED;
static obd_pair_t   s_pair;
static volatile bool s_probe_drop;     /* the link was dropped on purpose: the probed adapter answered, the user is asked */

static void addr_key(const ble_addr_t *a, uint8_t key[OBD_PAIR_KEY_LEN])
{
    key[0] = a->type;
    memcpy(key + 1, a->val, 6);
}

static ble_addr_t key_addr(const uint8_t key[OBD_PAIR_KEY_LEN])
{
    ble_addr_t a = {.type = key[0]};
    memcpy(a.val, key + 1, 6);
    return a;
}

static bool probing(const ble_addr_t *a)
{
    uint8_t k[OBD_PAIR_KEY_LEN];
    addr_key(a, k);
    portENTER_CRITICAL(&s_pair_mux);
    bool p = obd_pair_is_probing(&s_pair, k);
    portEXIT_CRITICAL(&s_pair_mux);
    return p;
}

static void probe_failed(void)
{
    portENTER_CRITICAL(&s_pair_mux);
    obd_pair_probe_failed(&s_pair);
    portEXIT_CRITICAL(&s_pair_mux);
}
#endif
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
/* Pairing safety (adapter_guard.h): the paired adapter's fingerprint (R1) and no silent re-pair (R2). The question and the
   records are shared like s_pair, under s_pair_mux; the actions (NVS, bonds, links, events) run after the lock. */
static ag_q_t        s_q;
static adapter_fp_t  s_fp_now;             /* this link's: the advert (host task), the discovery, the ELM replies (obd task) */
static adapter_fp_t  s_fp_saved;           /* NVS "fp", valid when s_fp_have */
static bool          s_fp_have;
static volatile bool s_fp_changed;         /* R1: this link's adapter looks different and the user has not said "Use it" */
static volatile bool s_fp_ask_due;         /* R1: ask when parked (the board task's 1 s poll opens the question) */
static volatile bool s_fp_trusted;         /* R1: this link matched or was learned, so its protocol may be added */
static volatile bool s_fp_checked;         /* R1: decided once per link (elm_initialise runs again while the ECU sleeps) */
static volatile bool s_bond_bad;           /* R2: this link's re-pair was refused or waits for the answer */
static volatile bool s_bond_hold;          /* R2: the link waits for the answer: no subscribe, no data */
static bool          s_bond_had, s_bond_our_had;
/* the saved adapter's bond when this link came up: host task and board task only, never with the cache off, so PSRAM */
EXT_RAM_BSS_ATTR static struct ble_store_value_sec s_bond_peer, s_bond_our;
static int64_t       s_hold_until_ms;      /* "Not this one", "Don't allow" or no answer: the saved address waits */
static int64_t       s_allow_until_ms;     /* "Allow": a pairing request is let through until then */
static uint32_t      s_meta_gen = 1, s_meta_done, s_meta_session;
static struct { uint16_t r1_changed, r2_seen, r2_refused, r2_allowed, r1_checks; } s_cnt;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool guard_driving(void) { return elm_drive_active() || sys_mode_get() == MODE_DRIVE; }
static bool guard_held(void)
{
    portENTER_CRITICAL(&s_pair_mux);
    bool h = now_ms() < s_hold_until_ms || s_q.kind == AG_Q_REPAIR;   /* a re-pair question: no new link meanwhile */
    portEXIT_CRITICAL(&s_pair_mux);
    return h;
}
static void fp_from_adv(const struct ble_gap_disc_desc *d, const struct ble_hs_adv_fields *f);
static void guard_link_up(void);
static bool guard_after_enc(void);
static int  guard_repeat_pairing(void);
#endif

static portMUX_TYPE s_reject_mux = portMUX_INITIALIZER_UNLOCKED;
static struct { ble_addr_t addr; int64_t until; } s_reject[N_REJECT];
static int s_reject_next;

/* ------------------------------------------------------------------ NVS */
static void load_saved(void)
{
    nvs_handle_t h;
    s_have_saved = false;
    if (nvs_open("obdble", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_saved_addr;
    if (nvs_get_blob(h, "addr", &s_saved_addr, &len) == ESP_OK && len == sizeof s_saved_addr) s_have_saved = true;
    len = sizeof s_cur_name;
    nvs_get_str(h, "name", s_cur_name, &len);
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
    len = sizeof s_fp_saved;
    s_fp_have = nvs_get_blob(h, "fp", &s_fp_saved, &len) == ESP_OK && len == sizeof s_fp_saved && afp_valid(&s_fp_saved);
#endif
    nvs_close(h);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
    snprintf(s_saved_name, sizeof s_saved_name, "%s", s_cur_name);
    ESP_LOGI(TAG, "OBD adapter: %s", s_have_saved ? "paired" : "not paired");
#else
    if (s_have_saved) ESP_LOGI(TAG, "saved adapter %s (%s)", addr_str(s_saved_addr.val), s_cur_name);
#endif
}

#if CONFIG_OBD_BLE_PAIR_CONFIRM
/* only obd_ble_pair_answer() reaches this, with the user's yes */
static void save_adapter(const ble_addr_t *a, const char *name)
{
    nvs_handle_t h;
    if (nvs_open("obdble", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "addr", a, sizeof *a);
    nvs_set_str(h, "name", name);
#if BOARD_MOSAICO
    nvs_erase_key(h, "fp");                /* a new pairing: the fingerprint is learned on its first link (adapter_guard.h) */
    portENTER_CRITICAL(&s_pair_mux);
    s_fp_have = false;
    portEXIT_CRITICAL(&s_pair_mux);
#endif
    nvs_commit(h);
    nvs_close(h);
    s_saved_addr = *a;
    snprintf(s_saved_name, sizeof s_saved_name, "%s", name);
    s_have_saved = true;
    ESP_LOGI(TAG, "OBD adapter paired; future boots auto-connect");
}
#else
static void save_current(void)
{
    nvs_handle_t h;
    if (nvs_open("obdble", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "addr", &s_cur_addr, sizeof s_cur_addr);
    nvs_set_str(h, "name", s_cur_name);
    nvs_commit(h);
    nvs_close(h);
    s_saved_addr = s_cur_addr;
    s_have_saved = true;
    ESP_LOGI(TAG, "adapter address saved; future boots auto-connect");
}
#endif

void obd_ble_forget_adapter(void)
{
    nvs_handle_t h;
    if (nvs_open("obdble", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_have_saved = false;
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
    portENTER_CRITICAL(&s_pair_mux);
    s_fp_have = false;                     /* nvs_erase_all took the fingerprint too */
    s_hold_until_ms = 0;
    ag_q_close(&s_q);                      /* a question about the forgotten adapter is void */
    portEXIT_CRITICAL(&s_pair_mux);
#endif
#if CONFIG_OBD_BLE_PAIR_CONFIRM
    s_saved_name[0] = 0;
    STATUS_LOCK(); g_status.adapter_saved = false; STATUS_UNLOCK();
#endif
    ble_store_clear();                     /* drop bonds too, so a new adapter pairs from scratch */
#if CONFIG_OBD_BLE_PAIR_CONFIRM
    ESP_LOGW(TAG, "OBD adapter forgotten: not paired");
#else
    ESP_LOGW(TAG, "saved adapter forgotten");
#endif
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

/* ------------------------------------------------------------------ skip list */
static bool is_rejected(const ble_addr_t *a)
{
    int64_t now = esp_timer_get_time();
    bool hit = false;
    portENTER_CRITICAL(&s_reject_mux);
    for (int i = 0; i < N_REJECT; i++)
        if (s_reject[i].until > now && ble_addr_cmp(&s_reject[i].addr, a) == 0) { hit = true; break; }
    portEXIT_CRITICAL(&s_reject_mux);
    return hit;
}

static void reject(const ble_addr_t *a)
{
    portENTER_CRITICAL(&s_reject_mux);
    s_reject[s_reject_next].addr = *a;
    s_reject[s_reject_next].until = esp_timer_get_time() + REJECT_TTL_US;
    s_reject_next = (s_reject_next + 1) % N_REJECT;
    portEXIT_CRITICAL(&s_reject_mux);
}

/* Drops the current link; an unsaved device is also skipped for REJECT_TTL_US so the scan moves on. */
static void drop_current(const char *why)
{
    bool saved = s_have_saved && ble_addr_cmp(&s_cur_addr, &s_saved_addr) == 0;
    if (!saved) {
        reject(&s_cur_addr);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
        probe_failed();
        sys_event(EV_WARN, "Skipping %s: %s", s_cur_name[0] ? s_cur_name : "a device", why);
#else
        sys_event(EV_WARN, "Skipping %s: %s", s_cur_name[0] ? s_cur_name : addr_str(s_cur_addr.val), why);
#endif
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

#if CONFIG_OBD_BLE_PAIR_CONFIRM
bool obd_ble_adapter_confirmed(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
    if (s_have_saved && ble_addr_cmp(&s_cur_addr, &s_saved_addr) == 0) {
        STATUS_LOCK(); g_status.adapter_saved = true; STATUS_UNLOCK();
        return true;
    }
    /* a new adapter answered like an ELM327: drop the link now and ask; nothing is saved or logged from it */
    portENTER_CRITICAL(&s_pair_mux);
    bool asked = obd_pair_probe_answered(&s_pair, esp_timer_get_time() / 1000, PAIR_ASK_MS);
    portEXIT_CRITICAL(&s_pair_mux);
    if (asked) ESP_LOGI(TAG, "a new OBD adapter answered; asking the user before pairing");
    s_probe_drop = true;
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    return false;
}
#else
void obd_ble_adapter_confirmed(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (!s_have_saved || ble_addr_cmp(&s_cur_addr, &s_saved_addr) != 0) save_current();
    STATUS_LOCK(); g_status.adapter_saved = s_have_saved; STATUS_UNLOCK();
}
#endif

void obd_ble_adapter_rejected(const char *why) { drop_current(why); }

/* ------------------------------------------------------------------ discovery */
static adapter_uuid_t to_adapter_uuid(const ble_uuid_any_t *u)
{
    adapter_uuid_t a = adapter_uuid16(0);
    switch (u->u.type) {
    case BLE_UUID_TYPE_16:
        a = adapter_uuid16(u->u16.value);
        break;
    case BLE_UUID_TYPE_32:
        a.b[0] = (uint8_t)(u->u32.value >> 24);
        a.b[1] = (uint8_t)(u->u32.value >> 16);
        a.b[2] = (uint8_t)(u->u32.value >> 8);
        a.b[3] = (uint8_t)u->u32.value;
        break;
    default:
        for (int i = 0; i < 16; i++) a.b[i] = u->u128.value[15 - i];   /* NimBLE stores 128-bit UUIDs little-endian */
        break;
    }
    return a;
}

static bool find_cccd(const struct peer_chr *chr, uint16_t *cccd)
{
    const struct peer_dsc *d;
    SLIST_FOREACH(d, &chr->dscs, next) {
        if (ble_uuid_cmp(&d->dsc.uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            *cccd = d->dsc.handle;
            return true;
        }
    }
    return false;
}

static bool resolve_handles(const struct peer *peer)
{
    static adapter_chr_t flat[MAX_CHRS];
    static const struct peer_chr *refs[MAX_CHRS];
    size_t n = 0;
    const struct peer_svc *svc;
    SLIST_FOREACH(svc, &peer->svcs, next) {
        adapter_uuid_t su = to_adapter_uuid(&svc->svc.uuid);
        const struct peer_chr *c;
        SLIST_FOREACH(c, &svc->chrs, next) {
            if (n >= MAX_CHRS) break;
            uint16_t cccd;
            uint8_t props = c->chr.properties;
            if (!find_cccd(c, &cccd)) props &= (uint8_t)~(ADAPTER_PROP_NOTIFY | ADAPTER_PROP_INDICATE);
            flat[n] = (adapter_chr_t){.svc = su, .chr = to_adapter_uuid(&c->chr.uuid), .props = props};
            refs[n++] = c;
        }
    }

    int ni, wi, profile;
    if (!adapter_choose(flat, n, CONFIG_OBD_BLE_SVC_UUID, CONFIG_OBD_BLE_NOTIFY_UUID, CONFIG_OBD_BLE_WRITE_UUID,
                        &ni, &wi, &profile)) return false;
    const struct peer_chr *nc = refs[ni], *wc = refs[wi];
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
    portENTER_CRITICAL(&s_pair_mux);
    afp_set_gatt(&s_fp_now, flat[ni].svc.b, flat[ni].chr.b, flat[wi].chr.b);
    portEXIT_CRITICAL(&s_pair_mux);
#endif
    find_cccd(nc, &s_cccd_handle);
    s_notify_handle = nc->chr.val_handle;
    s_write_handle  = wc->chr.val_handle;
    s_use_indicate  = !(nc->chr.properties & BLE_GATT_CHR_PROP_NOTIFY);
    s_write_no_rsp  = !(wc->chr.properties & BLE_GATT_CHR_PROP_WRITE);
    char sb[BLE_UUID_STR_LEN];
    ESP_LOGI(TAG, "serial line via %s profile, rx chr %s: rx=0x%04x (%s) tx=0x%04x (%s)", adapter_profile_name(profile),
             ble_uuid_to_str(&nc->chr.uuid.u, sb), s_notify_handle, s_use_indicate ? "indicate" : "notify",
             s_write_handle, s_write_no_rsp ? "write-no-rsp" : "write");
    return true;
}

/* ------------------------------------------------------------------ GAP */
static int gap_event(struct ble_gap_event *event, void *arg);

/* obd_ble_set_paused() stops the scan loop (an existing link is kept); obd_ble_probe() opens one short scan window
   while paused. Nothing in the logger pauses the receiver by default; the hooks stay for experiments. */
static volatile bool s_paused;
#if BOARD_MOSAICO && CONFIG_OCM_IDS
static bool s_scan_wl;                  /* host task: the running scan uses the accept list (the guard blocks) */
#endif

static void start_scan(void)
{
    s_best_valid = false;
    if (s_paused) { STATUS_LOCK(); g_status.ble = BLE_ST_IDLE; STATUS_UNLOCK(); return; }
    struct ble_gap_disc_params p = {0};
    p.filter_duplicates = 0;   /* keep reporting so a weak adapter is retried once it gets closer */
    p.passive = 0;          /* active: ask for scan responses so we see names */
    p.itvl = 0x60;          /* 60 ms */
    p.window = 0x30;        /* 30 ms: leaves radio time for Wi-Fi */
    int32_t dur = BLE_HS_FOREVER;
#if BOARD_MOSAICO && CONFIG_OCM_IDS
    /* The network guard blocks (a flood nearby): the scan is narrowed to the paired adapter through the controller's
       accept list, so the flood is dropped before the host sees it. Scanned in 30 s pieces then, so the restart at the
       end of each piece (DISC_COMPLETE) opens the scan again as soon as the block lifts. Nothing is transmitted. */
    s_scan_wl = ids_ble_blocking() && s_have_saved && ble_gap_wl_set(&s_saved_addr, 1) == 0;
    if (s_scan_wl) { p.filter_policy = BLE_HCI_SCAN_FILT_USE_WL; dur = 30000; }
#endif
    int rc = ble_gap_disc(s_own_addr_type, dur, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
    STATUS_LOCK(); g_status.ble = BLE_ST_SCANNING; STATUS_UNLOCK();
}

static bool advertises_serial(const struct ble_hs_adv_fields *f)
{
    adapter_uuid_t cfg_a;
    bool have_cfg = CONFIG_OBD_BLE_SVC_UUID[0] && adapter_uuid_parse(CONFIG_OBD_BLE_SVC_UUID, &cfg_a);
    for (int i = 0; i < f->num_uuids16; i++) {
        adapter_uuid_t u = adapter_uuid16(f->uuids16[i].value);
        if (adapter_service_is_obd(&u) || (have_cfg && adapter_uuid_eq(&u, &cfg_a))) return true;
    }
    for (int i = 0; i < f->num_uuids128; i++) {
        ble_uuid_any_t any = {.u128 = f->uuids128[i]};
        adapter_uuid_t u = to_adapter_uuid(&any);
        if (adapter_service_is_obd(&u) || (have_cfg && adapter_uuid_eq(&u, &cfg_a))) return true;
    }
    return false;
}

#if CONFIG_OBD_BLE_PAIR_CONFIRM
/* obd_pair_pick() decides: the saved adapter (connect), a new one to offer (probe, then ask), or nothing */
static obd_pick_t is_candidate(const struct ble_gap_disc_desc *d, const struct ble_hs_adv_fields *f)
{
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND && d->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
        d->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) return OBD_PICK_NONE;
    if (d->rssi < CONFIG_OBD_BLE_MIN_RSSI) return OBD_PICK_NONE;

    uint8_t key[OBD_PAIR_KEY_LEN];
    addr_key(&d->addr, key);
    obd_adv_t a = {.key = key, .rssi = d->rssi, .have_saved = s_have_saved,
                   .is_saved = s_have_saved && ble_addr_cmp(&d->addr, &s_saved_addr) == 0,
                   .user_search = esp_timer_get_time() < s_explore_until};
    if (!a.is_saved && (!a.have_saved || a.user_search) && d->rssi >= CONFIG_OBD_BLE_PAIR_MIN_RSSI) {
        a.failed = is_rejected(&d->addr);
        a.looks_obd = (f->name && f->name_len &&
                       (adapter_name_matches((const char *)f->name, f->name_len, CONFIG_OBD_BLE_NAME_PREFIXES) ||
                        adapter_name_looks_obd((const char *)f->name, f->name_len))) || advertises_serial(f);
    }
    portENTER_CRITICAL(&s_pair_mux);
    obd_pick_t pick = obd_pair_pick(&s_pair, &a, CONFIG_OBD_BLE_MIN_RSSI, CONFIG_OBD_BLE_PAIR_MIN_RSSI);
    portEXIT_CRITICAL(&s_pair_mux);
#if BOARD_MOSAICO
    if (pick == OBD_PICK_OFFER && ids_ble_blocking()) return OBD_PICK_NONE;   /* the guard blocks: no new-adapter offers */
#endif
    return pick;
}
#else
/* the car board's tested rule: the saved adapter only (outside the 90 s search), else any OBD-looking device at
   CONFIG_OBD_BLE_MIN_RSSI or stronger that is not skipped */
static bool is_candidate(const struct ble_gap_disc_desc *d, const struct ble_hs_adv_fields *f)
{
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND && d->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
        d->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) return false;
    if (d->rssi < CONFIG_OBD_BLE_MIN_RSSI) return false;

    bool exploring = esp_timer_get_time() < s_explore_until;
    if (s_have_saved && !exploring) return ble_addr_cmp(&d->addr, &s_saved_addr) == 0;
    if (is_rejected(&d->addr)) return false;

    if (f->name && f->name_len &&
        (adapter_name_matches((const char *)f->name, f->name_len, CONFIG_OBD_BLE_NAME_PREFIXES) ||
         adapter_name_looks_obd((const char *)f->name, f->name_len))) return true;
    return advertises_serial(f);
}
#endif

#if CONFIG_OBD_BLE_PAIR_CONFIRM
static void connect_to(const ble_addr_t *addr, const char *name, int8_t rssi, obd_pick_t pick)
#else
static void connect_to(const ble_addr_t *addr, const char *name, int8_t rssi)
#endif
{
    s_cur_addr = *addr;
    snprintf(s_cur_name, sizeof s_cur_name, "%s", name);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
    ESP_LOGI(TAG, "adapter '%s' rssi=%d: connecting (%s)", s_cur_name, rssi, pick == OBD_PICK_SAVED ? "paired" : "not paired, probe");
    if (pick == OBD_PICK_OFFER) {
        uint8_t key[OBD_PAIR_KEY_LEN];
        addr_key(addr, key);
        portENTER_CRITICAL(&s_pair_mux);
        obd_pair_probe(&s_pair, key, s_cur_name, rssi);
        portEXIT_CRITICAL(&s_pair_mux);
    }
#else
    ESP_LOGI(TAG, "adapter found %s '%s' rssi=%d; connecting", addr_str(s_cur_addr.val), s_cur_name, rssi);
#endif
    STATUS_LOCK();
    g_status.ble = BLE_ST_CONNECTING;
    g_status.adapter_rssi = rssi;
    snprintf(g_status.adapter_name, sizeof g_status.adapter_name, "%s", s_cur_name);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
    /* the session file keeps the paired adapter's address only; a probed device's is never kept */
    if (pick == OBD_PICK_SAVED) snprintf(g_status.adapter_addr, sizeof g_status.adapter_addr, "%s", addr_str(s_cur_addr.val));
    else g_status.adapter_addr[0] = 0;
#else
    snprintf(g_status.adapter_addr, sizeof g_status.adapter_addr, "%s", addr_str(s_cur_addr.val));
#endif
    STATUS_UNLOCK();

    s_best_valid = false;
    ble_gap_disc_cancel();
    int rc = ble_gap_connect(s_own_addr_type, &s_cur_addr, 10000, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
        if (pick == OBD_PICK_OFFER) probe_failed();
#endif
        start_scan();
    }
}

static void subscribe(uint16_t conn);

static bool needs_security(int status)
{
    return status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
           status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC) ||
           status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_KEY_SZ);
}

static int on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        if (needs_security(error->status) && !s_sec_tried) {
            /* the adapter wants an encrypted link: pair (Just Works) and subscribe again on ENC_CHANGE */
            s_sec_tried = true;
            ESP_LOGI(TAG, "adapter asks for pairing (status=%d); starting security", error->status);
            int rc = ble_gap_security_initiate(conn_handle);
            if (rc == 0) return 0;
            ESP_LOGE(TAG, "security initiate rc=%d", rc);
        }
        ESP_LOGE(TAG, "CCCD write failed status=%d", error->status);
        drop_current("could not subscribe to its serial line");
        return 0;
    }
#if CONFIG_OBD_BLE_PAIR_CONFIRM
    sys_event(EV_INFO, "Adapter linked: %s", s_cur_name[0] ? s_cur_name : "OBD adapter");
#else
    sys_event(EV_INFO, "Adapter linked: %s", s_cur_name[0] ? s_cur_name : addr_str(s_cur_addr.val));
#endif
    s_ready = true;
    STATUS_LOCK();
    g_status.ble = BLE_ST_ELM_INIT;
    g_status.adapter_saved = s_have_saved;
    STATUS_UNLOCK();
    elm_link_changed(true);
    return 0;
}

static void subscribe(uint16_t conn)
{
    uint8_t v[2] = {s_use_indicate ? 2 : 1, 0};
    int rc = ble_gattc_write_flat(conn, s_cccd_handle, v, sizeof v, on_subscribe, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD write rc=%d", rc);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void on_disc_complete(const struct peer *peer, int status, void *arg)
{
    if (status != 0 || !resolve_handles(peer)) {
        ESP_LOGW(TAG, "no serial service (discovery status %d)", status);
        drop_current("no serial service");
        return;
    }
    subscribe(peer->conn_handle);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_hs_adv_fields f;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
#if BOARD_MOSAICO
        IDS_HOOK(ids_hook_ble_adv(event->disc.addr.val, event->disc.addr.type));   /* a count and a bitmap bit, no more */
#endif
        if (ble_gap_conn_active()) return 0;   /* already connecting */
#if BOARD_MOSAICO && CONFIG_OCM_IDS
        if (ids_ble_blocking() != s_scan_wl && s_conn == BLE_HS_CONN_HANDLE_NONE) {   /* the verdict changed: restart */
            ble_gap_disc_cancel();
            start_scan();
            return 0;
        }
#endif
        const struct ble_gap_disc_desc *d = &event->disc;
        int64_t now = esp_timer_get_time();
#if CONFIG_OBD_BLE_PAIR_CONFIRM
        obd_pick_t pick = ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 ? is_candidate(d, &f) : OBD_PICK_NONE;
        if (pick != OBD_PICK_NONE) {
#else
        if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 && is_candidate(d, &f)) {
#endif
            char name[32] = "";
            if (f.name && f.name_len) snprintf(name, sizeof name, "%.*s", (int)f.name_len, (const char *)f.name);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
            if (pick == OBD_PICK_SAVED) {
#if BOARD_MOSAICO
                if (guard_held()) return 0;    /* the user said no to it, or a re-pair question is open */
                fp_from_adv(d, &f);
#endif
                if (!name[0]) snprintf(name, sizeof name, "%s", s_saved_name);
                connect_to(&d->addr, name, d->rssi, OBD_PICK_SAVED);
#else
            if (s_have_saved) {
                if (!name[0]) snprintf(name, sizeof name, "%s", s_cur_name);
                connect_to(&d->addr, name, d->rssi);
#endif
                return 0;
            }
            if (!s_best_valid) { s_window_start = now; s_best_rssi = INT8_MIN; }
            bool same = s_best_valid && ble_addr_cmp(&s_best_addr, &d->addr) == 0;
            if (!s_best_valid || d->rssi > s_best_rssi || same) {
                if (!same || name[0]) snprintf(s_best_name, sizeof s_best_name, "%s", name);
                s_best_addr = d->addr;
                s_best_rssi = d->rssi;
                s_best_valid = true;
            }
        }
        if (s_best_valid && now - s_window_start >= CANDIDATE_WINDOW_US) {
            ble_addr_t a = s_best_addr;
            char n[32];
            snprintf(n, sizeof n, "%s", s_best_name);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
            connect_to(&a, n, s_best_rssi, OBD_PICK_OFFER);
#else
            connect_to(&a, n, s_best_rssi);
#endif
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            sys_event(EV_WARN, "Adapter connect failed (status %d)", event->connect.status);
#if CONFIG_OBD_BLE_PAIR_CONFIRM
            if (probing(&s_cur_addr)) { probe_failed(); reject(&s_cur_addr); }   /* a new device that will not link: skip it */
#endif
            start_scan();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_sec_tried = false;
#if BOARD_MOSAICO
        IDS_HOOK(ids_hook_link(true));
#endif
#if CONFIG_OBD_BLE_PAIR_CONFIRM
        s_probe_drop = false;
#endif
        ble_hs_cfg.sm_io_cap = (s_pin_valid && ble_addr_cmp(&s_pin_addr, &s_cur_addr) == 0) ? BLE_SM_IO_CAP_KEYBOARD_ONLY
                                                                                          : BLE_SM_IO_CAP_NO_IO;
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
        guard_link_up();
#endif
        ESP_LOGI(TAG, "connected handle=%d", s_conn);
        STATUS_LOCK(); g_status.ble = BLE_ST_DISCOVERING; STATUS_UNLOCK();
        peer_add(s_conn);
        ble_gattc_exchange_mtu(s_conn, NULL, NULL);
        rc = peer_disc_all(s_conn, on_disc_complete, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "peer_disc_all rc=%d", rc);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
#if BOARD_MOSAICO
        IDS_HOOK(ids_hook_link(false));
#endif
#if CONFIG_OBD_BLE_PAIR_CONFIRM
        if (s_probe_drop) s_probe_drop = false;                    /* dropped on purpose: the user is being asked */
        else {
            if (s_ready) sys_event(EV_WARN, "OBD adapter disconnected (reason %d)", event->disconnect.reason);
            if (probing(&s_cur_addr)) { probe_failed(); reject(&s_cur_addr); }   /* lost before it answered */
        }
#else
        if (s_ready) sys_event(EV_WARN, "OBD adapter disconnected (reason %d)", event->disconnect.reason);
#endif
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
        portENTER_CRITICAL(&s_pair_mux);
        if (s_q.kind == AG_Q_CHANGED) ag_q_close(&s_q);              /* the next link is checked again */
        portEXIT_CRITICAL(&s_pair_mux);
        s_bond_hold = false;
#endif
        peer_delete(event->disconnect.conn.conn_handle);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ready = false;
        elm_link_changed(false);
        STATUS_LOCK(); memset(g_status.obd.valid, 0, sizeof g_status.obd.valid); STATUS_UNLOCK();
        start_scan();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption %s (status %d)", event->enc_change.status == 0 ? "on" : "failed", event->enc_change.status);
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
        if (event->enc_change.status == 0 && event->enc_change.conn_handle == s_conn && guard_after_enc()) return 0;
#endif
        if (s_sec_tried && !s_ready && event->enc_change.conn_handle == s_conn) {
            if (event->enc_change.status == 0) {
                subscribe(s_conn);
            } else if (ble_hs_cfg.sm_io_cap == BLE_SM_IO_CAP_NO_IO) {
                /* Just Works refused: try once more as a keyboard with the fixed PIN some adapters use
                   (OBDLink CX: "PIN for older BLE standards is 123456") */
                ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
                s_pin_addr = s_cur_addr;      /* remembered, in case the adapter drops the link after a failed pairing */
                s_pin_valid = true;
                if (ble_gap_security_initiate(s_conn) != 0) drop_current("pairing failed");
            } else {
                drop_current("pairing failed");
            }
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
        return guard_repeat_pairing();         /* never silently (adapter_guard.h R2) */
#else
        /* the adapter lost its bond (reset or re-paired elsewhere): drop ours and pair again */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
#endif
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* Just Works first; a numeric comparison is accepted, and a passkey request gets the fixed adapter PIN */
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1};
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_INPUT, .passkey = 123456};
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        } else {
            ESP_LOGW(TAG, "adapter wants passkey entry (action %d); not supported", event->passkey.params.action);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == s_notify_handle) {
            uint8_t buf[256];
            uint16_t len = 0;
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof buf, &len) == 0 && len) elm_rx_bytes(buf, len);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* scan ended (e.g. cancelled before connect); only restart if nothing is connected */
        if (s_conn == BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_active()) start_scan();
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ host */
static int on_write_done(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) ESP_LOGW(TAG, "write status=%d", error->status);
    if (s_write_done) xSemaphoreGive(s_write_done);
    return 0;
}

int obd_ble_write(const uint8_t *data, size_t len)
{
    if (!s_ready || s_conn == BLE_HS_CONN_HANDLE_NONE) return -1;
    uint16_t mtu = ble_att_mtu(s_conn);
    size_t chunk = mtu > 23 ? mtu - 3 : 20;   /* ATT payload is MTU minus the 3-byte header */
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        bool more = off + n < len;
        int rc;
        if (s_write_no_rsp) {
            rc = ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, data + off, n);
        } else {
            if (s_write_done) xSemaphoreTake(s_write_done, 0);
            rc = ble_gattc_write_flat(s_conn, s_write_handle, data + off, n, on_write_done, NULL);
            /* one ATT request at a time: wait for this piece before sending the next */
            if (rc == 0 && more && s_write_done && xSemaphoreTake(s_write_done, pdMS_TO_TICKS(1000)) != pdTRUE) rc = -1;
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "write rc=%d", rc);
            return rc;
        }
    }
    return 0;
}

bool obd_ble_connected(void) { return s_ready; }

#if CONFIG_OBD_BLE_PAIR_CONFIRM
void obd_ble_explore(bool user)
{
    if (!user) {
        /* the automatic search never replaces or bypasses a saved adapter, and keeps every skip */
        ESP_LOGI(TAG, "automatic adapter search: %s", s_have_saved ? "paired, only the paired adapter" : "not paired, strong signal only");
        return;
    }
    portENTER_CRITICAL(&s_reject_mux);
    memset(s_reject, 0, sizeof s_reject);
    portEXIT_CRITICAL(&s_reject_mux);
    portENTER_CRITICAL(&s_pair_mux);
    obd_pair_user_search(&s_pair);
    portEXIT_CRITICAL(&s_pair_mux);
    s_explore_until = esp_timer_get_time() + 90LL * 1000000;
    sys_event(EV_INFO, "Searching for an OBD adapter for 90 s");
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && !s_paused) {
        ble_gap_disc_cancel();
        start_scan();
    }
}

/* ------------------------------------------------------------------ the pairing question */
static void pair_finish(const obd_pair_result_t *r)
{
    if (r->done == OBD_DONE_NONE) return;
    ble_addr_t a = key_addr(r->key);
    if (r->done == OBD_DONE_SAVE) {
        ble_addr_t old = s_saved_addr;
        bool replaced = s_have_saved && ble_addr_cmp(&old, &a) != 0;
        save_adapter(&a, r->name);
        if (replaced) ble_store_util_delete_peer(&old);            /* switching: the previous adapter's bond goes */
        s_explore_until = 0;                                        /* the search is over: this is the paired adapter */
        STATUS_LOCK();
        g_status.adapter_saved = true;
        snprintf(g_status.adapter_name, sizeof g_status.adapter_name, "%s", r->name);
        STATUS_UNLOCK();
        sys_event(EV_INFO, "OBD adapter paired: %s", r->name[0] ? r->name : "OBD adapter");
        /* linked to another adapter meanwhile (the old one, during Find OBD): drop it so the paired one connects */
        if (s_conn != BLE_HS_CONN_HANDLE_NONE && ble_addr_cmp(&s_cur_addr, &a) != 0) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        ble_store_util_delete_peer(&a);                             /* a bond made during the probe is not kept */
        STATUS_LOCK();
        snprintf(g_status.adapter_name, sizeof g_status.adapter_name, "%s", s_have_saved ? s_saved_name : "");
        g_status.adapter_saved = s_have_saved;
        STATUS_UNLOCK();
        sys_event(EV_INFO, "OBD adapter not paired");
    }
}

void obd_ble_pair_answer(bool yes)
{
    portENTER_CRITICAL(&s_pair_mux);
    obd_pair_result_t r = obd_pair_answer(&s_pair, yes ? OBD_ANS_YES : OBD_ANS_NO, esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&s_pair_mux);
    pair_finish(&r);
}

#if BOARD_MOSAICO
static void guard_tick(void);
#endif
void obd_ble_pair_poll(void)
{
    portENTER_CRITICAL(&s_pair_mux);
    obd_pair_result_t r = obd_pair_tick(&s_pair, esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&s_pair_mux);
    pair_finish(&r);
#if BOARD_MOSAICO
    guard_tick();
#endif
}

bool obd_ble_pair_offer(char *name, size_t n, const char **strength, int *secs_left)
{
    obd_pair_t q;                                 /* copied under the lock, formatted outside it */
    portENTER_CRITICAL(&s_pair_mux);
    q = s_pair;
    portEXIT_CRITICAL(&s_pair_mux);
    return obd_pair_offer(&q, esp_timer_get_time() / 1000, name, n, strength, secs_left);
}

uint32_t obd_ble_pair_seq(void)
{
    portENTER_CRITICAL(&s_pair_mux);
    uint32_t q = s_pair.seq;
#if BOARD_MOSAICO
    q += s_q.seq;                                 /* a guard question wakes the screen too */
#endif
    portEXIT_CRITICAL(&s_pair_mux);
    return q;
}

#if BOARD_MOSAICO
/* ------------------------------------------------------------------ pairing safety (adapter_guard.h) */
static bool saved_link(void) { return s_have_saved && s_conn != BLE_HS_CONN_HANDLE_NONE && ble_addr_cmp(&s_cur_addr, &s_saved_addr) == 0; }

static void fp_store(const adapter_fp_t *fp)
{
    nvs_handle_t h;
    if (nvs_open("obdble", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "fp", fp, sizeof *fp);
        nvs_commit(h);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_pair_mux);
    s_fp_saved = *fp;
    s_fp_have = true;
    portEXIT_CRITICAL(&s_pair_mux);
}

/* the saved adapter's advert that led to this connect: its stable parts only (adapter_guard.h). Host task only. */
static void fp_from_adv(const struct ble_gap_disc_desc *d, const struct ble_hs_adv_fields *f)
{
    EXT_RAM_BSS_ATTR static uint16_t u16[AFP_MAX_UUIDS];     /* scratch, host task only: PSRAM, not internal RAM */
    EXT_RAM_BSS_ATTR static uint32_t u32[AFP_MAX_UUIDS];
    EXT_RAM_BSS_ATTR static uint8_t  u128[AFP_MAX_UUIDS][16];
    afp_adv_t a = {.name = f->name, .name_len = f->name_len, .mfg = f->mfg_data, .mfg_len = f->mfg_data_len,
                   .uuid16 = u16, .uuid32 = u32, .uuid128 = (const uint8_t (*)[16])u128};
    for (int i = 0; i < f->num_uuids16 && a.n16 < AFP_MAX_UUIDS; i++) u16[a.n16++] = f->uuids16[i].value;
    for (int i = 0; i < f->num_uuids32 && a.n32 < AFP_MAX_UUIDS; i++) u32[a.n32++] = f->uuids32[i].value;
    for (int i = 0; i < f->num_uuids128 && a.n128 < AFP_MAX_UUIDS; i++) memcpy(u128[a.n128++], f->uuids128[i].value, 16);
    adapter_fp_t fp;
    afp_init(&fp);
    afp_set_addr_type(&fp, d->addr.type);
    afp_set_adv(&fp, d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP, &a);
    portENTER_CRITICAL(&s_pair_mux);
    s_fp_now = fp;
    portEXIT_CRITICAL(&s_pair_mux);
}

static bool bond_key(struct ble_store_key_sec *k)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) != 0) return false;
    memset(k, 0, sizeof *k);
    k->peer_addr = desc.peer_id_addr;
    return true;
}

/* a new link (host task): nothing is trusted yet; the saved adapter's bond is copied so a re-pair can be seen (R2) */
static void guard_link_up(void)
{
    s_fp_changed = false;
    s_fp_ask_due = false;
    s_fp_trusted = false;
    s_fp_checked = false;
    s_bond_bad = false;
    s_bond_hold = false;
    s_bond_had = s_bond_our_had = false;
    s_meta_gen++;
    struct ble_store_key_sec k;
    if (!saved_link() || !bond_key(&k)) return;
    s_bond_had = ble_store_read_peer_sec(&k, &s_bond_peer) == 0 && s_bond_peer.ltk_present;
    s_bond_our_had = s_bond_had && ble_store_read_our_sec(&k, &s_bond_our) == 0;
}

static void bond_restore(void)
{
    if (!s_bond_had) return;
    ble_store_write_peer_sec(&s_bond_peer);
    if (s_bond_our_had) ble_store_write_our_sec(&s_bond_our);
}

static void bond_keep_new(void)
{
    struct ble_store_key_sec k;
    if (!bond_key(&k)) return;
    s_bond_had = ble_store_read_peer_sec(&k, &s_bond_peer) == 0 && s_bond_peer.ltk_present;
    s_bond_our_had = s_bond_had && ble_store_read_our_sec(&k, &s_bond_our) == 0;
}

static ag_r2_t r2_decide(bool *driving)
{
    int64_t now = now_ms();
    *driving = guard_driving();
    portENTER_CRITICAL(&s_pair_mux);
    bool busy = s_q.kind != AG_Q_NONE || s_pair.phase == OBD_PAIR_ASKING;
    ag_r2_t r = ag_r2_decide(*driving, now < s_allow_until_ms, busy);
    if (r == AG_R2_ASK && ids_ble_blocking()) r = AG_R2_REFUSE;   /* the guard blocks: no re-pair request reaches the user */
    if (r == AG_R2_ASK && !ag_q_open(&s_q, AG_Q_REPAIR, now, PAIR_ASK_MS)) r = AG_R2_REFUSE;
    if (r == AG_R2_ALLOW) s_allow_until_ms = 0;            /* one pairing per "Allow" */
    portEXIT_CRITICAL(&s_pair_mux);
    s_cnt.r2_seen++;
    if (r == AG_R2_ALLOW) s_cnt.r2_allowed++;
    if (r == AG_R2_REFUSE) s_cnt.r2_refused++;
    return r;
}

/* After an encryption (host task): did the saved adapter get a new key? NimBLE re-pairs by itself when the saved key is
   refused (adapter_guard.h R2). True: stop here, the link waits for the user's answer. */
static bool guard_after_enc(void)
{
    if (!s_bond_had || !saved_link()) return false;
    struct ble_store_key_sec k;
    struct ble_store_value_sec v;
    if (!bond_key(&k) || ble_store_read_peer_sec(&k, &v) != 0) return false;
    if (v.ltk_present && !memcmp(v.ltk, s_bond_peer.ltk, sizeof v.ltk) && v.ediv == s_bond_peer.ediv &&
        v.rand_num == s_bond_peer.rand_num) return false;             /* the saved key: restored, not paired again */
    bool driving;
    ag_r2_t r = r2_decide(&driving);
    s_meta_gen++;
    if (r == AG_R2_ALLOW) {
        bond_keep_new();
        sys_event(EV_INFO, "Adapter paired again (allowed)");
        return false;
    }
    if (r == AG_R2_REFUSE) {
        bond_restore();                                                /* the new key is not kept; the link stays, marked */
        s_bond_bad = true;
        sys_event(EV_WARN, "Re-pair refused (%s), %u so far", driving ? "driving" : "busy", (unsigned)s_cnt.r2_refused);
        return false;
    }
    s_bond_bad = true;
    s_bond_hold = true;
    sys_event(EV_WARN, "Adapter asks to pair again: asking");
    return true;
}

/* BLE_GAP_EVENT_REPEAT_PAIRING: NimBLE 6.1 does not send it to a central; the same table in case a stack does */
static int guard_repeat_pairing(void)
{
    bool driving;
    ag_r2_t r = r2_decide(&driving);
    if (r == AG_R2_ALLOW) {
        struct ble_store_key_sec k;
        if (bond_key(&k)) ble_store_util_delete_peer(&k.peer_addr);
        sys_event(EV_INFO, "Adapter paired again (allowed)");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    if (r == AG_R2_REFUSE) sys_event(EV_WARN, "Re-pair refused (%s), %u so far", driving ? "driving" : "busy", (unsigned)s_cnt.r2_refused);
    else sys_event(EV_WARN, "Adapter asks to pair again: asking");
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
}

void obd_ble_guard_elm(const char *ati, const char *at1)
{
    if (!saved_link() || s_fp_checked) return;
    s_fp_checked = true;
    adapter_fp_t now, saved;
    portENTER_CRITICAL(&s_pair_mux);
    afp_set_elm(&s_fp_now, AFP_ATI, ati);
    afp_set_elm(&s_fp_now, AFP_AT1, at1);
    now = s_fp_now;
    saved = s_fp_saved;
    bool have = s_fp_have;
    portEXIT_CRITICAL(&s_pair_mux);
    uint16_t diff;
    bool driving = guard_driving();
    ag_r1_t r = ag_r1_decide(have ? &saved : NULL, &now, driving, &diff);
    s_cnt.r1_checks++;
    unsigned long id = (unsigned long)afp_id(&now);
    s_meta_gen++;
    switch (r) {
    case AG_R1_LEARN:
        fp_store(&now);
        s_fp_trusted = true;
        sys_event(EV_INFO, "Adapter fingerprint saved (%08lx)", id);
        break;
    case AG_R1_USE:
        s_fp_trusted = true;
        ESP_LOGI(TAG, "adapter fingerprint %08lx: same as when paired", id);
        break;
    default:
        s_fp_changed = true;
        s_fp_ask_due = true;
        s_cnt.r1_changed++;
        sys_event(EV_WARN, "Adapter looks different (%s)", afp_field_name(afp_first(diff & ~AFP_SOFT)));
        ESP_LOGW(TAG, "adapter fingerprint %08lx, paired as %08lx: rows marked, nothing to the learner, %s", id,
                 (unsigned long)afp_id(&saved), driving ? "asking once parked" : "asking");
        break;
    }
}

void obd_ble_guard_protocol(const char *dpn)
{
    if (!saved_link()) return;
    portENTER_CRITICAL(&s_pair_mux);
    bool known = afp_set_proto(&s_fp_now, dpn);
    uint32_t h = s_fp_now.h[AFP_PROTO];
    bool had = s_fp_have && (s_fp_saved.known & (1u << AFP_PROTO));
    bool differs = had && known && s_fp_saved.h[AFP_PROTO] != h;
    adapter_fp_t add = s_fp_saved;
    portEXIT_CRITICAL(&s_pair_mux);
    if (differs) {
        ESP_LOGI(TAG, "adapter fingerprint: another protocol than when paired (another car?), noted only");
    } else if (known && !had && s_fp_trusted && s_fp_have) {
        add.h[AFP_PROTO] = h;                          /* a trusted link: the protocol joins the record (soft field) */
        add.known |= 1u << AFP_PROTO;
        fp_store(&add);
    }
}

bool obd_ble_adapter_changed(void) { return s_fp_changed || s_bond_bad || ids_rows_suspect(); }   /* + the network guard */
void obd_ble_guard_counts(uint32_t *fp_checks, uint32_t *repair_refused)
{
    *fp_checks = s_cnt.r1_checks;
    *repair_refused = s_cnt.r2_refused;
}

void obd_ble_guard_meta(void)
{
    uint32_t sess = sdlog_session_id();
    if (s_meta_done == s_meta_gen && s_meta_session == sess) return;
    s_meta_done = s_meta_gen;
    s_meta_session = sess;
    portENTER_CRITICAL(&s_pair_mux);
    unsigned long id = (unsigned long)afp_id(&s_fp_now);
    portEXIT_CRITICAL(&s_pair_mux);
    char j[160];
    snprintf(j, sizeof j, "{\"result\":\"%s\",\"fp\":\"%08lx\",\"repair\":\"%s\",\"repair_events\":%u,\"changed_links\":%u}",
             s_fp_changed ? "changed" : s_fp_trusted ? "same" : "unchecked", id,
             s_bond_hold ? "asking" : s_bond_bad ? "refused" : "none", (unsigned)s_cnt.r2_seen, (unsigned)s_cnt.r1_changed);
    sdlog_meta_set_json("adapter_check", j);
}

bool obd_ble_guard_offer(const char **text, const char **yes, const char **no, const char **note, int *secs_left, uint32_t *seq)
{
    ag_q_kind_t k;
    int left;
    uint32_t sq;
    portENTER_CRITICAL(&s_pair_mux);
    bool open = ag_q_offer(&s_q, now_ms(), &k, &left, &sq);
    portEXIT_CRITICAL(&s_pair_mux);
    if (!open) return false;
    if (text) *text = ag_q_text(k);
    if (yes) *yes = ag_q_yes(k);
    if (no) *no = ag_q_no(k);
    if (note) *note = k == AG_Q_CHANGED ? "Its data is kept apart until you choose." : "Nothing changes unless you choose.";
    if (secs_left) *secs_left = left;
    if (seq) *seq = sq & 0x7FFFFFFFu;
    return true;
}

/* an answer, or none in time (yes = false, timeout = true). Board task (NVS) or the 1 s poll. */
static void guard_finish(ag_q_kind_t k, bool yes, bool timeout)
{
    int64_t now = now_ms();
    s_meta_gen++;
    if (k == AG_Q_CHANGED) {
        if (yes) {
            portENTER_CRITICAL(&s_pair_mux);
            adapter_fp_t fp = s_fp_now;
            portEXIT_CRITICAL(&s_pair_mux);
            fp_store(&fp);                             /* re-learned: only ever after this yes */
            s_fp_changed = false;
            s_fp_trusted = true;
            sys_event(EV_INFO, "Adapter accepted (%08lx)", (unsigned long)afp_id(&fp));
        } else if (timeout) {
            sys_event(EV_INFO, "No answer: adapter data kept apart");
        } else {
            portENTER_CRITICAL(&s_pair_mux);
            s_hold_until_ms = now + AG_HOLD_MS;
            portEXIT_CRITICAL(&s_pair_mux);
            sys_event(EV_INFO, "Adapter not used for 10 min");
            if (saved_link()) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    if (k != AG_Q_REPAIR) return;
    if (yes) {
        if (s_bond_hold) {                             /* NimBLE already paired: keep the new key and go on */
            s_cnt.r2_allowed++;
            bond_keep_new();
            s_bond_hold = false;
            s_bond_bad = false;
            sys_event(EV_INFO, "Adapter paired again (allowed)");
            if (saved_link() && !s_ready) subscribe(s_conn);
        } else {
            portENTER_CRITICAL(&s_pair_mux);
            s_allow_until_ms = now + AG_ALLOW_MS;
            portEXIT_CRITICAL(&s_pair_mux);
            sys_event(EV_INFO, "Pairing again allowed for 2 min");
            if (saved_link()) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    s_cnt.r2_refused++;
    bond_restore();                                    /* the key from before stays the saved one */
    s_bond_hold = false;
    portENTER_CRITICAL(&s_pair_mux);
    s_hold_until_ms = now + AG_HOLD_MS;
    portEXIT_CRITICAL(&s_pair_mux);
    sys_event(EV_INFO, "Re-pair %s: old key kept", timeout ? "not answered" : "refused");
    if (saved_link()) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

void obd_ble_guard_answer(uint32_t seq, bool yes)
{
    portENTER_CRITICAL(&s_pair_mux);
    uint32_t cur = s_q.seq & 0x7FFFFFFFu;
    ag_q_kind_t k = cur == (seq & 0x7FFFFFFFu) ? ag_q_answer(&s_q, s_q.seq, now_ms()) : AG_Q_NONE;
    portEXIT_CRITICAL(&s_pair_mux);
    if (k == AG_Q_NONE) { ESP_LOGI(TAG, "late answer to an adapter question: ignored"); return; }
    guard_finish(k, yes, false);
}

/* the 1 s poll: a question whose time ran out, a question that must not stay open while driving, and the "looks
   different" question once parked */
static void guard_tick(void)
{
    int64_t now = now_ms();
    bool driving = guard_driving();
    bool linked = saved_link();
    portENTER_CRITICAL(&s_pair_mux);
    ag_q_kind_t expired = ag_q_tick(&s_q, now);
    bool closed = false;
    if (s_q.kind == AG_Q_CHANGED && driving) { ag_q_close(&s_q); closed = true; }
    bool asked = !driving && linked && s_fp_ask_due && s_fp_changed && s_pair.phase != OBD_PAIR_ASKING &&
                 ag_q_open(&s_q, AG_Q_CHANGED, now, PAIR_ASK_MS);
    portEXIT_CRITICAL(&s_pair_mux);
    if (closed) { s_fp_ask_due = true; ESP_LOGI(TAG, "adapter question closed for the drive; asked again once parked"); }
    if (asked) { s_fp_ask_due = false; ESP_LOGI(TAG, "asking whether to use the different-looking adapter"); }
    if (expired != AG_Q_NONE) guard_finish(expired, false, true);
}
#endif
#else
void obd_ble_explore(void)
{
    portENTER_CRITICAL(&s_reject_mux);
    memset(s_reject, 0, sizeof s_reject);
    portEXIT_CRITICAL(&s_reject_mux);
    s_explore_until = esp_timer_get_time() + 90LL * 1000000;
    sys_event(EV_INFO, "Searching for an OBD adapter for 90 s");
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && !s_paused) {
        ble_gap_disc_cancel();
        start_scan();
    }
}
#endif

void obd_ble_set_paused(bool paused)
{
    if (paused == s_paused) return;
    s_paused = paused;
    if (paused) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE && ble_gap_disc_active()) ble_gap_disc_cancel();
        STATUS_LOCK(); if (g_status.ble == BLE_ST_SCANNING) g_status.ble = BLE_ST_IDLE; STATUS_UNLOCK();
    } else if (s_conn == BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_active()) {
        start_scan();
    }
}

void obd_ble_probe(uint32_t ms)
{
    if (!s_paused || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_active() || ble_gap_disc_active()) return;
    struct ble_gap_disc_params p = {0};
    p.passive = 0;
    p.itvl = 0x60;
    p.window = 0x30;
    int rc = ble_gap_disc(s_own_addr_type, (int32_t)ms, &p, gap_event, NULL);   /* ends by itself; no restart while paused */
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "probe scan rc=%d", rc);
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset reason=%d", reason); }

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_scan();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t obd_ble_start(void)
{
#if CONFIG_OBD_BLE_PAIR_CONFIRM
    obd_pair_init(&s_pair);
#endif
    load_saved();
    STATUS_LOCK();
    g_status.adapter_saved = s_have_saved;
    if (s_have_saved) {
        snprintf(g_status.adapter_name, sizeof g_status.adapter_name, "%s", s_cur_name);
        snprintf(g_status.adapter_addr, sizeof g_status.adapter_addr, "%s", addr_str(s_saved_addr.val));
    }
    STATUS_UNLOCK();

    s_write_done = xSemaphoreCreateBinary();
    elm_init();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* Just Works pairing with bonding, used only when an adapter rejects an unencrypted subscribe */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 32, 32, 64, 64);
#else
    peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 32, 64, 64);
#endif
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
/* the Health app's adapter question (rule R1's "looks different"), at the end of the file so the S3's line numbers stay */
bool obd_ble_adapter_mismatch(void) { return s_fp_changed; }
#endif
