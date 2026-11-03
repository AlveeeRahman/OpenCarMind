/* NimBLE central for an ELM327-style BLE adapter (LELink / LELink2 and clones).
 *
 * First run: scan for an advertiser whose name starts with an entry of CONFIG_OBD_BLE_NAME_PREFIXES
 * or that advertises the configured service UUID, connect, and store its address in NVS.
 * Later runs: connect only to the stored address (auto-connect). Long-press BOOT clears it.
 */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
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

static uint8_t   s_own_addr_type;
static uint16_t  s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t  s_write_handle, s_notify_handle, s_cccd_handle;
static bool      s_write_no_rsp;
static bool      s_ready;

static ble_addr_t s_saved_addr;
static bool       s_have_saved;
static char       s_cur_name[32];
static ble_addr_t s_cur_addr;

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
    ESP_LOGW(TAG, "saved adapter forgotten");
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

/* ------------------------------------------------------------------ UUID helpers */
static bool parse_uuid(const char *s, ble_uuid_any_t *out)
{
    char hex[40];
    size_t n = 0;
    for (; *s && n + 1 < sizeof hex; s++) if (*s != '-' && *s != ' ') hex[n++] = *s;
    hex[n] = 0;
    if (n == 4) {
        memset(out, 0, sizeof *out);
        out->u16.u.type = BLE_UUID_TYPE_16;
        out->u16.value = (uint16_t)strtoul(hex, NULL, 16);
        return true;
    }
    if (n == 32) {
        uint8_t raw[16];
        for (int i = 0; i < 16; i++) {
            char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
            raw[15 - i] = (uint8_t)strtoul(b, NULL, 16);   /* NimBLE stores 128-bit UUIDs little-endian */
        }
        return ble_uuid_init_from_buf(out, raw, 16) == 0;
    }
    return false;
}

typedef struct { const char *svc, *ntf, *wr; } profile_t;
static const profile_t PROFILES[] = {
    {CONFIG_OBD_BLE_SVC_UUID, CONFIG_OBD_BLE_NOTIFY_UUID, CONFIG_OBD_BLE_WRITE_UUID},  /* configured (LELink: FFE0/FFE1/FFE1) */
    {"FFE0", "FFE1", "FFE1"},
    {"FFF0", "FFF1", "FFF2"},
    {"18F0", "2AF0", "2AF1"},
    {"6E400001-B5A3-F393-E0A9-E50E24DCCA9E", "6E400003-B5A3-F393-E0A9-E50E24DCCA9E", "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"},
};

static const struct peer_chr *find_cccd_chr(const struct peer *peer, const struct peer_chr *chr, uint16_t *cccd)
{
    const struct peer_dsc *d;
    SLIST_FOREACH(d, &chr->dscs, next) {
        if (ble_uuid_cmp(&d->dsc.uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            *cccd = d->dsc.handle;
            return chr;
        }
    }
    return NULL;
}

static bool resolve_handles(const struct peer *peer)
{
    /* 1. known profiles */
    for (size_t i = 0; i < sizeof PROFILES / sizeof PROFILES[0]; i++) {
        ble_uuid_any_t su, nu, wu;
        if (!PROFILES[i].svc[0] || !parse_uuid(PROFILES[i].svc, &su) ||
            !parse_uuid(PROFILES[i].ntf, &nu) || !parse_uuid(PROFILES[i].wr, &wu)) continue;
        const struct peer_chr *n = peer_chr_find_uuid(peer, &su.u, &nu.u);
        const struct peer_chr *w = peer_chr_find_uuid(peer, &su.u, &wu.u);
        if (!n || !w) continue;
        if (!(n->chr.properties & BLE_GATT_CHR_PROP_NOTIFY)) continue;
        if (!find_cccd_chr(peer, n, &s_cccd_handle)) continue;
        s_notify_handle = n->chr.val_handle;
        s_write_handle  = w->chr.val_handle;
        s_write_no_rsp  = !(w->chr.properties & BLE_GATT_CHR_PROP_WRITE) && (w->chr.properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP);
        ESP_LOGI(TAG, "profile %s matched: notify=0x%04x write=0x%04x cccd=0x%04x (%s)", PROFILES[i].svc,
                 s_notify_handle, s_write_handle, s_cccd_handle, s_write_no_rsp ? "write-no-rsp" : "write-with-rsp");
        return true;
    }
    /* 2. heuristic: first vendor service that has a NOTIFY chr and a WRITE chr */
    const struct peer_svc *svc;
    SLIST_FOREACH(svc, &peer->svcs, next) {
        if (svc->svc.uuid.u.type == BLE_UUID_TYPE_16 && svc->svc.uuid.u16.value >= 0x1800 && svc->svc.uuid.u16.value <= 0x18FF) continue;
        const struct peer_chr *n = NULL, *w = NULL, *c;
        SLIST_FOREACH(c, &svc->chrs, next) {
            if (!n && (c->chr.properties & BLE_GATT_CHR_PROP_NOTIFY)) n = c;
            if (!w && (c->chr.properties & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP))) w = c;
        }
        if (n && w && find_cccd_chr(peer, n, &s_cccd_handle)) {
            s_notify_handle = n->chr.val_handle;
            s_write_handle  = w->chr.val_handle;
            s_write_no_rsp  = !(w->chr.properties & BLE_GATT_CHR_PROP_WRITE);
            char buf[BLE_UUID_STR_LEN];
            ESP_LOGW(TAG, "no known profile; using service %s notify=0x%04x write=0x%04x",
                     ble_uuid_to_str(&svc->svc.uuid.u, buf), s_notify_handle, s_write_handle);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ GAP */
static int gap_event(struct ble_gap_event *event, void *arg);

/* obd_ble_set_paused() stops the scan loop (an existing link is kept); obd_ble_probe() opens one short scan window
   while paused. Nothing in the logger pauses the receiver by default; the hooks stay for experiments. */
static volatile bool s_paused;

static void start_scan(void)
{
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

static bool should_connect(const struct ble_gap_disc_desc *d, const struct ble_hs_adv_fields *f)
{
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND && d->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
        d->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) return false;
    if (d->rssi < CONFIG_OBD_BLE_MIN_RSSI) return false;

    if (s_have_saved) return ble_addr_cmp(&d->addr, &s_saved_addr) == 0;

    if (f->name && adapter_name_matches((const char *)f->name, f->name_len, CONFIG_OBD_BLE_NAME_PREFIXES)) return true;

    ble_uuid_any_t su;
    if (parse_uuid(CONFIG_OBD_BLE_SVC_UUID, &su) && su.u.type == BLE_UUID_TYPE_16) {
        for (int i = 0; i < f->num_uuids16; i++) if (f->uuids16[i].value == su.u16.value) return true;
    }
    return false;
}

static int on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(TAG, "CCCD write failed status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    sys_event(EV_INFO, "Adapter linked: %s", s_cur_name[0] ? s_cur_name : addr_str(s_cur_addr.val));
    if (!s_have_saved) save_current();
    s_ready = true;
    STATUS_LOCK();
    g_status.ble = BLE_ST_ELM_INIT;
    g_status.adapter_saved = s_have_saved;
    STATUS_UNLOCK();
    elm_link_changed(true);
    return 0;
}

static void on_disc_complete(const struct peer *peer, int status, void *arg)
{
    if (status != 0 || !resolve_handles(peer)) {
        sys_event(EV_ERROR, "Adapter has no serial service (status %d)", status);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    uint8_t v[2] = {1, 0};
    int rc = ble_gattc_write_flat(peer->conn_handle, s_cccd_handle, v, sizeof v, on_subscribe, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD write rc=%d", rc);
        ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_hs_adv_fields f;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (ble_gap_conn_active()) return 0;   /* already connecting */
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0) return 0;
        if (!should_connect(&event->disc, &f)) return 0;

        s_cur_addr = event->disc.addr;
        if (f.name && f.name_len) {
            size_t n = f.name_len < sizeof s_cur_name - 1 ? f.name_len : sizeof s_cur_name - 1;
            memcpy(s_cur_name, f.name, n);
            s_cur_name[n] = 0;
        }
        ESP_LOGI(TAG, "adapter found %s '%s' rssi=%d; connecting", addr_str(s_cur_addr.val), s_cur_name, event->disc.rssi);
        STATUS_LOCK();
        g_status.ble = BLE_ST_CONNECTING;
        g_status.adapter_rssi = event->disc.rssi;
        snprintf(g_status.adapter_name, sizeof g_status.adapter_name, "%s", s_cur_name);
        snprintf(g_status.adapter_addr, sizeof g_status.adapter_addr, "%s", addr_str(s_cur_addr.val));
        STATUS_UNLOCK();

        ble_gap_disc_cancel();
        rc = ble_gap_connect(s_own_addr_type, &s_cur_addr, 10000, NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            sys_event(EV_WARN, "Adapter connect failed (status %d)", event->connect.status);
            start_scan();
            return 0;
        }
        s_conn = event->connect.conn_handle;
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
    return 0;
}

int obd_ble_write(const uint8_t *data, size_t len)
{
    if (!s_ready || s_conn == BLE_HS_CONN_HANDLE_NONE) return -1;
    int rc = s_write_no_rsp ? ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, data, len)
                            : ble_gattc_write_flat(s_conn, s_write_handle, data, len, on_write_done, NULL);
    if (rc != 0) ESP_LOGW(TAG, "write rc=%d", rc);
    return rc;
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

    elm_init();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;

#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 32, 32, 64, 64);
#else
    peer_init(MYNEWT_VAL(BLE_MAX_CONNECTIONS), 32, 64, 64);
#endif
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
