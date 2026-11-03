/* NimBLE central for ELM327-style Bluetooth LE OBD-II adapters.
 *
 * First run: scan for adapters (by advertised name, by advertised serial service, or by the configured prefixes),
 * collect candidates for a short window, connect to the strongest, find its serial line, and keep its address only
 * after it answers the ELM327 reset. Devices that turn out not to be adapters are skipped for a while.
 * Later runs: connect only to the stored address. Long-press BOOT clears it.
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

static const char *TAG = "obd_ble";

void ble_store_config_init(void);

#define CANDIDATE_WINDOW_US  (1500 * 1000LL)   /* how long to collect candidates before picking the strongest */
#define REJECT_TTL_US        (10 * 60 * 1000000LL)
#define N_REJECT             8
#define MAX_CHRS             48

static uint8_t   s_own_addr_type;
static uint16_t  s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t  s_write_handle, s_notify_handle, s_cccd_handle;
static bool      s_write_no_rsp, s_use_indicate;
static bool      s_ready;
static bool      s_sec_tried;
static bool      s_pin_valid;          /* this address refused Just Works: pair it with the fixed PIN */
static ble_addr_t s_pin_addr;
static SemaphoreHandle_t s_write_done;

static ble_addr_t s_saved_addr;
static bool       s_have_saved;
static char       s_cur_name[32];
static ble_addr_t s_cur_addr;

/* best candidate seen in the current scan window */
static bool       s_best_valid;
static ble_addr_t s_best_addr;
static char       s_best_name[32];
static int8_t     s_best_rssi;
static int64_t    s_window_start;

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
    nvs_close(h);
    if (s_have_saved) ESP_LOGI(TAG, "saved adapter %s (%s)", addr_str(s_saved_addr.val), s_cur_name);
}

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

void obd_ble_forget_adapter(void)
{
    nvs_handle_t h;
    if (nvs_open("obdble", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_have_saved = false;
    ble_store_clear();                     /* drop bonds too, so a new adapter pairs from scratch */
    ESP_LOGW(TAG, "saved adapter forgotten");
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
        sys_event(EV_WARN, "Skipping %s: %s", s_cur_name[0] ? s_cur_name : addr_str(s_cur_addr.val), why);
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

void obd_ble_adapter_confirmed(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (!s_have_saved || ble_addr_cmp(&s_cur_addr, &s_saved_addr) != 0) save_current();
    STATUS_LOCK(); g_status.adapter_saved = s_have_saved; STATUS_UNLOCK();
}

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

static void start_scan(void)
{
    s_best_valid = false;
    if (s_paused) { STATUS_LOCK(); g_status.ble = BLE_ST_IDLE; STATUS_UNLOCK(); return; }
    struct ble_gap_disc_params p = {0};
    p.filter_duplicates = 0;   /* keep reporting so a weak adapter is retried once it gets closer */
    p.passive = 0;          /* active: ask for scan responses so we see names */
    p.itvl = 0x60;          /* 60 ms */
    p.window = 0x30;        /* 30 ms: leaves radio time for Wi-Fi */
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
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

static bool is_candidate(const struct ble_gap_disc_desc *d, const struct ble_hs_adv_fields *f)
{
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND && d->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
        d->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) return false;
    if (d->rssi < CONFIG_OBD_BLE_MIN_RSSI) return false;

    if (s_have_saved) return ble_addr_cmp(&d->addr, &s_saved_addr) == 0;
    if (is_rejected(&d->addr)) return false;

    if (f->name && f->name_len &&
        (adapter_name_matches((const char *)f->name, f->name_len, CONFIG_OBD_BLE_NAME_PREFIXES) ||
         adapter_name_looks_obd((const char *)f->name, f->name_len))) return true;
    return advertises_serial(f);
}

static void connect_to(const ble_addr_t *addr, const char *name, int8_t rssi)
{
    s_cur_addr = *addr;
    snprintf(s_cur_name, sizeof s_cur_name, "%s", name);
    ESP_LOGI(TAG, "adapter found %s '%s' rssi=%d; connecting", addr_str(s_cur_addr.val), s_cur_name, rssi);
    STATUS_LOCK();
    g_status.ble = BLE_ST_CONNECTING;
    g_status.adapter_rssi = rssi;
    snprintf(g_status.adapter_name, sizeof g_status.adapter_name, "%s", s_cur_name);
    snprintf(g_status.adapter_addr, sizeof g_status.adapter_addr, "%s", addr_str(s_cur_addr.val));
    STATUS_UNLOCK();

    s_best_valid = false;
    ble_gap_disc_cancel();
    int rc = ble_gap_connect(s_own_addr_type, &s_cur_addr, 10000, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
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
    sys_event(EV_INFO, "Adapter linked: %s", s_cur_name[0] ? s_cur_name : addr_str(s_cur_addr.val));
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
        if (ble_gap_conn_active()) return 0;   /* already connecting */
        const struct ble_gap_disc_desc *d = &event->disc;
        int64_t now = esp_timer_get_time();
        if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 && is_candidate(d, &f)) {
            char name[32] = "";
            if (f.name && f.name_len) snprintf(name, sizeof name, "%.*s", (int)f.name_len, (const char *)f.name);
            if (s_have_saved) {
                if (!name[0]) snprintf(name, sizeof name, "%s", s_cur_name);
                connect_to(&d->addr, name, d->rssi);
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
            connect_to(&a, n, s_best_rssi);
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            sys_event(EV_WARN, "Adapter connect failed (status %d)", event->connect.status);
            start_scan();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_sec_tried = false;
        ble_hs_cfg.sm_io_cap = (s_pin_valid && ble_addr_cmp(&s_pin_addr, &s_cur_addr) == 0) ? BLE_SM_IO_CAP_KEYBOARD_ONLY
                                                                                          : BLE_SM_IO_CAP_NO_IO;
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
        if (s_ready) sys_event(EV_WARN, "OBD adapter disconnected (reason %d)", event->disconnect.reason);
        peer_delete(event->disconnect.conn.conn_handle);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ready = false;
        elm_link_changed(false);
        STATUS_LOCK(); memset(g_status.obd.valid, 0, sizeof g_status.obd.valid); STATUS_UNLOCK();
        start_scan();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption %s (status %d)", event->enc_change.status == 0 ? "on" : "failed", event->enc_change.status);
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
        /* the adapter lost its bond (reset or re-paired elsewhere): drop ours and pair again */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
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
