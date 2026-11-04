#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include "ml_infer.h"
#include "rev_task.h"
#include "sched_mon.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "elm327.h"
#include "obd_ble.h"
#include "econ.h"
#include "status.h"
#include "sdlog.h"
#include "custom_pids.h"
#include "fault_engine.h"
#include "pattern.h"
#include "events.h"
#include "remote_config.h"
#include "esp_attr.h"

static const char *TAG = "elm";

#define LINK_UP_BIT   BIT0
#define SLOW_PER_CYCLE 3            /* slow PIDs polled per cycle, round-robin */
#define RAW_EVERY_SLOW_PASSES CONFIG_OBD_RAW_EVERY_PASSES   /* undecoded supported PIDs: raw-logged every Nth pass through the slow list */

static StreamBufferHandle_t s_rx;
static EventGroupHandle_t   s_ev;
static volatile bool        s_link_up;
static volatile bool        s_halt;        /* the user asked to stop (BOOT held for power-off): no polling, no new drive */
static volatile bool        s_in_drive;    /* the poll loop is running */
#define ECU_QUIET_US        (15 * 1000000LL)   /* no answer for this long ends the drive (ignition off) */
static SemaphoreHandle_t    s_txn_lock;

/* ------------------------------------------------------------------ field table */
typedef struct {
    const char *csv; const char *unit; uint8_t pid; uint8_t off; obd_kind_t kind; float scale; float offset; bool fast;
} field_def_t;
#define OBD_ROW(id, csv, unit, pid, off, kind, scale, offs, fast) { csv, unit, pid, off, kind, scale, offs, fast },
static const field_def_t FIELDS[F_COUNT] = { OBD_FIELDS(OBD_ROW) };
#undef OBD_ROW

EXT_RAM_BSS_ATTR static pattern_model_t s_pattern;      /* ~12 KB, in PSRAM */
static bool s_pattern_loaded;
#define PATTERNS_PATH SDLOG_DIR "/patterns.json"

static void pattern_load(void)
{
    EXT_RAM_BSS_ATTR static char buf[12288];
    pattern_init(&s_pattern);
    if (sdlog_load_text(PATTERNS_PATH, buf, sizeof buf) && pattern_model_from_json(&s_pattern, buf))
        ESP_LOGI(TAG, "pattern model loaded: %u centroids, %lu windows", s_pattern.used, (unsigned long)s_pattern.n_windows);
    else ESP_LOGI(TAG, "pattern model: starting fresh");
    s_pattern_loaded = true;
}

static void pattern_save(void)
{
    EXT_RAM_BSS_ATTR static char buf[12288];
    if (pattern_model_to_json(&s_pattern, buf, sizeof buf) > 0) sdlog_save_text_atomic(PATTERNS_PATH, buf);
}

/* One sample per logged row: rpm, speed, load, maf, throttle, stft, ltft, coolant, iat, map. */
static void pattern_step(const obd_values_t *v)
{
    if (!s_pattern_loaded) pattern_load();
    static const obd_field_t fields[PATTERN_SIGNALS] = {F_rpm, F_speed, F_load, F_maf, F_throttle, F_stft1, F_ltft1, F_coolant, F_iat, F_map};
    float sig[PATTERN_SIGNALS];
    uint16_t valid = 0;
    for (int i = 0; i < PATTERN_SIGNALS; i++) {
        sig[i] = v->f[fields[i]];
        if (OBD_HAS(v, fields[i])) valid |= 1 << i;
    }
    pattern_result_t r;
    if (pattern_feed(&s_pattern, esp_timer_get_time() / 1000, sig, valid, &r)) {
        sdlog_write_pattern_window(r.window_seq, r.start_ms, r.end_ms, r.samples, r.pattern_id, r.dist, r.novelty, r.spawned,
                                   r.sax_rpm, r.sax_speed, r.features, PATTERN_FEATURES);
        sdlog_set_window_seq(r.window_seq);
        ml_window_closed(r.window_seq, r.start_ms, r.end_ms);   /* hands the input to the inference task, never waits */
        STATUS_LOCK();
        g_status.pattern_id = r.pattern_id;
        g_status.pattern_novelty = r.novelty;
        g_status.pattern_windows = s_pattern.n_windows;
        g_status.pattern_used = s_pattern.used;
        STATUS_UNLOCK();
        if (r.window_seq % 6 == 0) pattern_save();      /* once a minute */
    }
}

static custom_pid_t s_custom[CUSTOM_PID_MAX];
static size_t       s_custom_n;
static char         s_default_header[12];   /* "7DF" or "18DB33F1" when protocol is CAN, else "" */
static char         s_cur_header[12];

/* ------------------------------------------------------------------ transport glue */
void elm_rx_bytes(const uint8_t *data, size_t len)
{
    if (s_rx) xStreamBufferSend(s_rx, data, len, 0);
}

void elm_link_changed(bool up)
{
    s_link_up = up;
    if (s_rx) xStreamBufferReset(s_rx);
    if (up) xEventGroupSetBits(s_ev, LINK_UP_BIT);
}

int elm_transact(const char *cmd, char *resp, size_t resp_sz, uint32_t timeout_ms)
{
    if (!s_link_up) return -2;
    /* bounded: one transaction holds the lock for at most timeout_ms, so waiting longer than that means it is stuck */
    if (xSemaphoreTake(s_txn_lock, pdMS_TO_TICKS(timeout_ms + 1000)) != pdTRUE) {
        ESP_LOGW(TAG, "ELM busy, '%s' skipped", cmd);
        return -1;
    }
    xStreamBufferReset(s_rx);

    char tx[48];
    int n = snprintf(tx, sizeof tx, "%s\r", cmd);
    if (obd_ble_write((const uint8_t *)tx, n) != 0) {
        xSemaphoreGive(s_txn_lock);
        return -2;
    }

    size_t used = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    bool got_prompt = false;
    while (!got_prompt) {
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0 || !s_link_up) break;
        uint8_t chunk[64];
        size_t r = xStreamBufferReceive(s_rx, chunk, sizeof chunk, pdMS_TO_TICKS(left / 1000 + 1));
        for (size_t i = 0; i < r; i++) {
            char c = (char)chunk[i];
            if (c == '>') { got_prompt = true; break; }
            if (c == '\0') continue;
            if (used + 1 < resp_sz) resp[used++] = c;
        }
    }
    resp[used] = 0;
    xSemaphoreGive(s_txn_lock);
    sched_kick();             /* a bounded step finished: the poll cycle is slow at worst, not hung */

    if (!got_prompt) {
        ESP_LOGW(TAG, "timeout on '%s' (got '%s')", cmd, resp);
        return s_link_up ? -1 : -2;
    }
    size_t cl = strlen(cmd);
    if (strncasecmp(resp, cmd, cl) == 0) memmove(resp, resp + cl, used - cl + 1);
    char *w = resp;
    bool last_nl = true;
    for (char *r = resp; *r; r++) {
        if (*r == '\r' || *r == '\n') {
            if (!last_nl) { *w++ = '\n'; last_nl = true; }
        } else { *w++ = *r; last_nl = false; }
    }
    while (w > resp && w[-1] == '\n') w--;
    *w = 0;
    return (int)(w - resp);
}

/* ------------------------------------------------------------------ parsing helpers */
static bool resp_is_error(const char *r)
{
    return strstr(r, "NO DATA") || strstr(r, "UNABLE") || strstr(r, "ERROR") || strstr(r, "STOPPED") ||
           strchr(r, '?') || strstr(r, "BUFFER FULL") || strstr(r, "BUS INIT") || strstr(r, "SEARCHING") == r;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Collapse an ELM reply (ATH0, ATS0) into one hex string, handling the multi-line ISO-TP form
 *   "014\n0: 490201314433\n1: ...". Returns the hex (uppercase, no spaces) in out. */
static void collapse_hex(const char *resp, char *out, size_t out_sz)
{
    size_t h = 0;
    const char *line = resp;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t L = eol ? (size_t)(eol - line) : strlen(line);
        const char *colon = memchr(line, ':', L);
        size_t start = colon ? (size_t)(colon - line) + 1 : 0;
        bool bare_len = (!colon && L <= 3);           /* "014" frame-length line */
        if (!bare_len)
            for (size_t i = start; i < L && h + 1 < out_sz; i++)
                if (!isspace((unsigned char)line[i])) out[h++] = (char)toupper((unsigned char)line[i]);
        line = eol ? eol + 1 : NULL;
    }
    out[h] = 0;
}

/* Find prefix in the collapsed reply and convert the following hex to bytes. Returns count or -1. */
static int bytes_after_prefix(const char *resp, const char *prefix, uint8_t *out, size_t max, char *rawhex, size_t rawhex_sz)
{
    char hex[600];
    collapse_hex(resp, hex, sizeof hex);
    const char *p = strstr(hex, prefix);
    if (!p) return -1;
    p += strlen(prefix);
    if (rawhex) snprintf(rawhex, rawhex_sz, "%s", p);
    size_t n = 0;
    while (p[0] && p[1] && n < max) {
        int hi = hexval(p[0]), lo = hexval(p[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)(hi << 4 | lo);
        p += 2;
    }
    return (int)n;
}

/* ELM327 datasheet, "Additional OBD request characters": a single hex digit after the request is the maximum number of
   responses to wait for, so the chip returns as soon as it has them instead of waiting out the AT ST timer (200 ms here).
   Used only on ISO 15765-4 CAN after a probe (probe_resp_count); J1850 buses need every response acknowledged. */
static bool s_resp_count;

static int query_pid(uint8_t pid, uint8_t *b, size_t max, char *rawhex, size_t rawhex_sz)
{
    char cmd[8], resp[256], prefix[8];
    snprintf(cmd, sizeof cmd, "01%02X%s", pid, s_resp_count ? "1" : "");
    snprintf(prefix, sizeof prefix, "41%02X", pid);
    int rc = elm_transact(cmd, resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS);
    if (rc == -2) return -2;
    if (rc <= 0 || resp_is_error(resp)) return -1;
    return bytes_after_prefix(resp, prefix, b, max, rawhex, rawhex_sz);
}

/* ------------------------------------------------------------------ supported PIDs */
static uint8_t s_supported[32];      /* 256 bits, bit set = ECU reports PID supported */
static bool pid_supported(uint8_t pid) { return (s_supported[pid / 8] >> (7 - pid % 8)) & 1; }

static void read_supported(void)
{
    memset(s_supported, 0, sizeof s_supported);
    uint8_t b[8];
    for (int base = 0x00; base <= 0xC0; base += 0x20) {
        char rawhex[16];
        int n = query_pid((uint8_t)base, b, 4, rawhex, sizeof rawhex);
        if (n < 4) break;
        char cmd[8];
        snprintf(cmd, sizeof cmd, "01%02X", base);
        rawhex[8] = 0;
        sdlog_meta_set_supported(cmd, rawhex);
        for (int i = 0; i < 32; i++)
            if ((b[i / 8] >> (7 - i % 8)) & 1) {
                int pid = base + 1 + i;
                s_supported[pid / 8] |= 1 << (7 - pid % 8);
            }
        if (!(b[3] & 1)) break;
    }
    int cnt = 0;
    for (int p = 1; p < 256; p++) cnt += pid_supported(p);
    ESP_LOGI(TAG, "%d supported mode-01 PIDs", cnt);
}

/* ------------------------------------------------------------------ decoding into g_status */
static float raw_value(const field_def_t *d, const uint8_t *b, int n)
{
    int o = d->off;
    switch (d->kind) {
    case K_U8:  return n > o ? b[o] : NAN;
    case K_U16: return n > o + 1 ? (float)(b[o] * 256 + b[o + 1]) : NAN;
    case K_S16: return n > o + 1 ? (float)(int16_t)(b[o] << 8 | b[o + 1]) : NAN;
    case K_U32: return n > o + 3 ? (float)((uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]) : NAN;
    }
    return NAN;
}

/* Query one PID and store every table field it carries. Returns false when nothing decoded. */
static bool poll_pid(uint8_t pid)
{
    uint8_t b[16];
    int n = query_pid(pid, b, sizeof b, NULL, 0);
    if (n <= 0) return false;
    bool any = false;
    STATUS_LOCK();
    for (int i = 0; i < F_COUNT; i++) {
        if (FIELDS[i].pid != pid) continue;
        float raw = raw_value(&FIELDS[i], b, n);
        if (raw != raw) continue;   /* NAN: short reply */
        /* lambda sensors 0x24..0x2B report 0xFFFF for "not used"; O2 voltage PIDs 0xFF too */
        g_status.obd.f[i] = raw * FIELDS[i].scale + FIELDS[i].offset;
        OBD_SET(&g_status.obd, i);
        any = true;
    }
    STATUS_UNLOCK();
    return any;
}

/* Coolant, STFT and LTFT are model inputs; they join the fast tier when requests are cheap */
static bool pid_is_model_channel(uint8_t pid) { return pid == 0x05 || pid == 0x06 || pid == 0x07; }

static void probe_resp_count(const char *protocol)
{
    s_resp_count = false;
    size_t L = strlen(protocol);
    if (L == 0 || !strchr("6789", protocol[L - 1]) || !pid_supported(0x0C)) return;   /* ATDPN 6..9 = ISO 15765-4 CAN */
    s_resp_count = true;
    uint8_t b[8];
    if (query_pid(0x0C, b, sizeof b, NULL, 0) < 2) s_resp_count = false;
    ESP_LOGI(TAG, "response-count requests %s", s_resp_count ? "on" : "off");
}

static bool pid_in_table(uint8_t pid)
{
    for (int i = 0; i < F_COUNT; i++) if (FIELDS[i].pid == pid) return true;
    return false;
}

static bool pid_is_fast(uint8_t pid)
{
    for (int i = 0; i < F_COUNT; i++) if (FIELDS[i].pid == pid && FIELDS[i].fast) return true;
    return false;
}

static void set_header(const char *hdr)
{
    if (!hdr[0] || strcmp(hdr, s_cur_header) == 0) return;
    char cmd[24], resp[64];
    snprintf(cmd, sizeof cmd, "ATSH%s", hdr);
    if (elm_transact(cmd, resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) snprintf(s_cur_header, sizeof s_cur_header, "%s", hdr);
}

static void poll_custom(size_t idx)
{
    custom_pid_t *c = &s_custom[idx];
    if (c->header[0]) {
        if (!s_default_header[0]) return;        /* header switching only on CAN */
        set_header(c->header);
    } else if (s_cur_header[0] && strcmp(s_cur_header, s_default_header) != 0) {
        set_header(s_default_header);
    }
    char resp[256];
    int rc = elm_transact(c->cmd, resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS);
    if (rc <= 0 || resp_is_error(resp)) return;
    uint8_t b[64];
    int n = bytes_after_prefix(resp, c->prefix, b, sizeof b, NULL, 0);
    if (n <= 0) return;
    float v;
    if (!custom_pid_eval(c->formula, b, n, &v)) return;
    STATUS_LOCK();
    g_status.obd.custom[idx] = v;
    g_status.obd.custom_valid |= 1 << idx;
    STATUS_UNLOCK();
}

static void poll_dtcs(void)
{
    uint8_t b[64];
    char raw[8];
    int n = query_pid(0x01, b, 4, raw, sizeof raw);
    if (n >= 1) {
        STATUS_LOCK();
        g_status.obd.mil_on = b[0] & 0x80;
        g_status.obd.dtc_count = b[0] & 0x7F;
        g_status.obd.status_valid = true;
        STATUS_UNLOCK();
    }
    static const struct { const char *cmd; const char *prefix; size_t off; } modes[] = {
        {"03", "43", offsetof(obd_values_t, dtcs)},
        {"07", "47", offsetof(obd_values_t, pending_dtcs)},
        {"0A", "4A", offsetof(obd_values_t, permanent_dtcs)},
    };
    for (size_t m = 0; m < 3; m++) {
        char resp[256];
        int rc = elm_transact(modes[m].cmd, resp, sizeof resp, 4000);
        if (rc <= 0 || resp_is_error(resp)) continue;
        n = bytes_after_prefix(resp, modes[m].prefix, b, sizeof b, NULL, 0);
        char codes[64] = "";
        if (n >= 1) {
            int start = (n % 2) ? 1 : 0;     /* CAN replies carry a count byte first */
            dtc_bytes_to_string(b + start, n - start, codes, sizeof codes);
        }
        char *dst = (char *)&g_status.obd + modes[m].off;
        STATUS_LOCK();
        bool changed = strcmp(dst, codes) != 0;
        snprintf(dst, 64, "%s", codes);
        STATUS_UNLOCK();
        if (changed && m == 0) sdlog_meta_add_dtcs(esp_timer_get_time() / 1000, codes);
    }

    /* Rank and group everything we now know. */
    EXT_RAM_BSS_ATTR static fault_report_t rep;
    obd_values_t snap;
    STATUS_LOCK(); snap = g_status.obd; STATUS_UNLOCK();
    bool was_mil = g_status.faults.mil_on;
    fault_engine_analyse(snap.dtcs, snap.pending_dtcs, snap.permanent_dtcs, snap.mil_on, &rep);
    STATUS_LOCK();
    bool differs = rep.n != g_status.faults.n || strcmp(rep.insight, g_status.faults.insight) != 0;
    g_status.faults = rep;
    if (differs) g_status.fault_rev++;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (rep.new_serious > 0) {
        const fault_t *f = &rep.faults[0];
        snprintf(g_status.alarm_text, sizeof g_status.alarm_text, "%s %s", f->code, f->desc);
        g_status.alarm_critical = f->severity >= SEV_CRITICAL;
        g_status.alarm_until_ms = g_status.alarm_critical ? INT64_MAX : now_ms + 8000;
    } else if (snap.mil_on && !was_mil && snap.status_valid) {
        snprintf(g_status.alarm_text, sizeof g_status.alarm_text, "Check engine light is on");
        g_status.alarm_critical = false;
        g_status.alarm_until_ms = now_ms + 8000;
    }
    STATUS_UNLOCK();
    if (differs) {
        char ranked[128] = "";
        size_t u = 0;
        for (int i = 0; i < rep.n && u + 7 < sizeof ranked; i++) u += snprintf(ranked + u, sizeof ranked - u, "%s%s", i ? " " : "", rep.faults[i].code);
        sdlog_meta_set_faults(rep.insight, ranked, rep.max_severity);
    }
}

/* ------------------------------------------------------------------ init sequence */
/* Tries each OBD-II protocol explicitly after ATSP0 failed, in python-OBD's order (CAN first). ATTP keeps the
   auto-search fallback; clones that do not know ATTP get ATSP. Restores ATSP0 when none answers. */
static bool try_protocols(char *resp, size_t sz)
{
    static const char order[] = "681792345";
    for (const char *p = order; *p && s_link_up; p++) {
        char cmd[8];
        snprintf(cmd, sizeof cmd, "ATTP%c", *p);
        if (elm_transact(cmd, resp, sz, CONFIG_OBD_ELM_TIMEOUT_MS) < 0) return false;
        if (strchr(resp, '?')) {
            snprintf(cmd, sizeof cmd, "ATSP%c", *p);
            if (elm_transact(cmd, resp, sz, CONFIG_OBD_ELM_TIMEOUT_MS) < 0) return false;
        }
        int rc = elm_transact("0100", resp, sz, 8000);
        if (rc > 0 && strstr(resp, "4100")) {
            ESP_LOGI(TAG, "ECU answered on protocol %c", *p);
            return true;
        }
    }
    elm_transact("ATSP0", resp, sz, CONFIG_OBD_ELM_TIMEOUT_MS);
    return false;
}

static bool elm_initialise(void)
{
    char resp[256];
    int rc;
    STATUS_LOCK(); g_status.ble = BLE_ST_ELM_INIT; STATUS_UNLOCK();
    s_cur_header[0] = 0;
    s_default_header[0] = 0;

    s_resp_count = false;
    char batch[16] = "";
    if (elm_transact("!?", resp, sizeof resp, 1500) > 0 && strncmp(resp, "VER", 3) == 0) snprintf(batch, sizeof batch, "%.8s", resp);

    rc = elm_transact("ATZ", resp, sizeof resp, 4000);
    if (rc == -1) rc = elm_transact("ATZ", resp, sizeof resp, 6000);   /* slow clones need a second, longer reset */
    if (rc < 0) {
        sys_event(EV_ERROR, "OBD adapter not responding (ATZ)");
        if (rc == -1) obd_ble_adapter_rejected("no ELM327 reply");
        return false;
    }
    obd_ble_adapter_confirmed();           /* it answered with a prompt: an ELM327-style adapter, keep it */
    {
        const char *t = resp;
        while (*t == ' ' || *t == '\n') t++;
        if (strncmp(t, "NA", 2) == 0 && (t[2] == 0 || t[2] == '\n' || t[2] == ' ')) {
            sys_event(EV_ERROR, "Adapter locked: disable LELink password");
            STATUS_LOCK(); g_status.ble = BLE_ST_LOCKED; STATUS_UNLOCK();
            return false;
        }
    }
    char ati[32];
    snprintf(ati, sizeof ati, "%.31s", resp);
    vTaskDelay(pdMS_TO_TICKS(500));

    static const char *init_cmds[] = {"ATE0", "ATL0", "ATS0", "ATH0", "ATAT1", "ATST32", "ATSP0"};
    for (size_t i = 0; i < sizeof init_cmds / sizeof init_cmds[0]; i++) {
        rc = elm_transact(init_cmds[i], resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS);
        if (rc < 0) { sys_event(EV_ERROR, "OBD adapter stopped answering (%s)", init_cmds[i]); return false; }
        ESP_LOGI(TAG, "%s -> %s", init_cmds[i], resp);
    }
    if (elm_transact("ATI", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) snprintf(ati, sizeof ati, "%.31s", resp);
    app_status_t st;
    status_get(&st);

    int tries = 0;
    do {
        rc = elm_transact("0100", resp, sizeof resp, 12000);
        if (rc > 0 && strstr(resp, "4100")) break;
        ESP_LOGW(TAG, "0100 -> '%s' (ignition off?)", resp);
        /* auto search gave up although the adapter answers: try each protocol in turn, as python-OBD does */
        if (rc > 0 && (tries == 2 || tries == 10) && (strstr(resp, "UNABLE") || strstr(resp, "ERROR")) &&
            try_protocols(resp, sizeof resp)) break;
        vTaskDelay(pdMS_TO_TICKS(3000));
    } while (++tries < 20 && s_link_up && !s_halt);
    if (!s_link_up || s_halt || tries >= 20) { if (s_link_up && !s_halt) sys_event(EV_WARN, "ECU not answering: ignition off?"); return false; }

    sdlog_begin_drive();                   /* the ECU answered: a new session if the last drive was closed */
    sdlog_meta_set_adapter(st.adapter_name, st.adapter_addr, ati, batch);

    char protocol[24] = "";
    if (elm_transact("ATDPN", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) snprintf(protocol, sizeof protocol, "%.23s", resp);
    {
        char pn = protocol[0] == 'A' ? protocol[1] : protocol[0];   /* "A6" = auto-detected protocol 6 */
        if (pn == '6' || pn == '8') strcpy(s_default_header, "7DF");
        else if (pn == '7' || pn == '9') strcpy(s_default_header, "18DB33F1");
    }

    char vin[18] = "";
    if (elm_transact("0902", resp, sizeof resp, 6000) > 0 && !resp_is_error(resp)) {
        uint8_t raw[40];
        int n = bytes_after_prefix(resp, "490201", raw, sizeof raw, NULL, 0);
        if (n >= 17) { memcpy(vin, raw, 17); vin[17] = 0; }
    }
    ESP_LOGI(TAG, "protocol=%s vin=%s", protocol, vin);
    sys_event(EV_INFO, "ECU online: %s", protocol);
    STATUS_LOCK();
    snprintf(g_status.obd.vin, sizeof g_status.obd.vin, "%s", vin);
    snprintf(g_status.obd.protocol, sizeof g_status.obd.protocol, "%s", protocol);
    STATUS_UNLOCK();
    sdlog_meta_set_vehicle(vin, protocol);

    read_supported();
    probe_resp_count(protocol);
    fault_engine_reset();

    s_custom_n = custom_pids_load(SDLOG_DIR "/custom_pids.csv", s_custom, CUSTOM_PID_MAX);
    STATUS_LOCK();
    g_status.obd.custom_count = s_custom_n;
    g_status.obd.custom_valid = 0;
    for (size_t i = 0; i < s_custom_n; i++) {
        snprintf(g_status.obd.custom_name[i], sizeof g_status.obd.custom_name[i], "%s", s_custom[i].name);
        snprintf(g_status.obd.custom_unit[i], sizeof g_status.obd.custom_unit[i], "%s", s_custom[i].unit);
    }
    STATUS_UNLOCK();
    for (size_t i = 0; i < s_custom_n; i++)
        sdlog_meta_set_custom(i, s_custom[i].cmd, s_custom[i].name, s_custom[i].unit, s_custom[i].formula, s_custom[i].header);
    return true;
}

/* ------------------------------------------------------------------ main task */
/* The drive is over (ignition off, link lost or the user is powering off): save and close the session, stop the jobs that
   expect rows, and leave the logging state so the power line, the USB door and the upload see a parked car. */
static void end_drive(sched_job_t poll_job)
{
    sched_set_expected(poll_job, false);
    ml_drive_ended();
    rev_drive_ended();
    sdlog_end_drive();
    STATUS_LOCK();
    if (g_status.ble == BLE_ST_LOGGING) g_status.ble = BLE_ST_ELM_INIT;
    memset(g_status.obd.valid, 0, sizeof g_status.obd.valid);
    STATUS_UNLOCK();
}

void elm_request_stop(void) { s_halt = true; }

bool elm_wait_stopped(uint32_t timeout_ms)
{
    s_halt = true;
    for (uint32_t t = 0; s_in_drive && t < timeout_ms; t += 50) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_in_drive) sdlog_end_drive();    /* also covers a stop before any drive started */
    return !s_in_drive;
}

void elm_resume(void)
{
    s_halt = false;
    if (s_link_up) xEventGroupSetBits(s_ev, LINK_UP_BIT);
}

static void obd_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(s_ev, LINK_UP_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s_halt) continue;                                     /* stopped for power-off: wait for elm_resume() */

        if (!elm_initialise()) {
            if (s_link_up) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                if (s_link_up) xEventGroupSetBits(s_ev, LINK_UP_BIT);
            }
            continue;
        }

        /* Build the polling lists from what the ECU supports. */
        uint8_t fast[32], slow[160], raw[160];
        int nf = 0, ns = 0, nr = 0;
        for (int pid = 0x01; pid < 0x100; pid++) {
            if (!pid_supported(pid) || (pid & 0x1F) == 0) continue;
            if (pid == 0x01) continue;                       /* status: handled with DTCs */
            /* R.E.V's airflow constant needs absolute load (0x43) in every row, like MAF and rpm: one more ELM transaction per
               cycle, only while R.E.V is the selected model and only on cars that report 0x43. Calculated load (0x04) is fast already. */
            bool rev_fast = pid == 0x43 && rev_wants_abs_load();
            if (pid_in_table(pid)) { if (pid_is_fast(pid) || (s_resp_count && pid_is_model_channel(pid)) || rev_fast) fast[nf++] = pid; else slow[ns++] = pid; }
            else if (nr < 160) raw[nr++] = pid;
        }
        ESP_LOGI(TAG, "polling %d fast, %d slow, %d raw-only PIDs, %u custom", nf, ns, nr, (unsigned)s_custom_n);

        STATUS_LOCK();
        g_status.ble = BLE_ST_LOGGING;
        memset(g_status.obd.valid, 0, sizeof g_status.obd.valid);
        STATUS_UNLOCK();
        ml_reset_history();
        static sched_job_t poll_job = -1;
        if (poll_job < 0) poll_job = sched_register("obd_poll", 1000, 30000, true);
        sched_set_expected(poll_job, true);

        int slow_i = 0, raw_i = 0, slow_passes = 0;
        int64_t last_dtc = 0, last_rv = 0;
        int consecutive_fail = 0;
        int64_t last_ok = esp_timer_get_time();
        econ_trip_t trip, trip_raw;                       /* calibrated (shown) and as the sources gave it */
        econ_reset(&trip);
        econ_reset(&trip_raw);
        int64_t last_row_us = 0;
        int64_t equiv_at_us = 0;                          /* when PID 0x44 was last read in this drive */
        float equiv_thr = -1;                             /* throttle at that read, -1 unknown */
        int64_t drive_start_ms = esp_timer_get_time() / 1000;
        STATUS_LOCK();                                    /* a new drive starts with no fuel or economy figures */
        g_status.obd.fuel_lph_calc = -1; g_status.obd.fuel_src = 0; g_status.obd.trip_km = 0; g_status.obd.trip_fuel_l = 0;
        g_status.obd.econ_trip_kpl = -1; g_status.obd.econ_inst_kpl = -1;
        g_status.obd.fuel_lph_raw = -1; g_status.obd.fuel_cal = rev_cal_factor(); g_status.obd.trip_fuel_raw_l = 0;
        STATUS_UNLOCK();
        rev_drive_started();
        sdlog_meta_set_ml(ml_model_key(ml_model_active()), rev_cal_factor());
        char resp[64];
        s_in_drive = true;

        while (s_link_up && !s_halt) {
            sched_begin(poll_job);
            bool any_ok = false;
            for (int i = 0; i < nf && s_link_up && !s_halt; i++) any_ok |= poll_pid(fast[i]);
            /* a few slow PIDs per cycle, round robin; custom PIDs share the rotation */
            int total_slow = ns + (int)s_custom_n;
            for (int k = 0; k < SLOW_PER_CYCLE && total_slow > 0 && s_link_up && !s_halt; k++) {
                int idx = (slow_i++) % total_slow;
                if (idx < ns) {
                    bool ok = poll_pid(slow[idx]);
                    if (ok && slow[idx] == 0x44) {
                        equiv_at_us = esp_timer_get_time();
                        STATUS_LOCK(); equiv_thr = OBD_HAS(&g_status.obd, F_throttle) ? g_status.obd.f[F_throttle] : -1; STATUS_UNLOCK();
                    }
                    any_ok |= ok;
                }
                else poll_custom(idx - ns);
                if (slow_i % total_slow == 0) slow_passes++;
            }
            if (s_cur_header[0] && strcmp(s_cur_header, s_default_header) != 0) set_header(s_default_header);
            /* raw-only PIDs: one per cycle, only every Nth pass, so the fast loop stays quick */
            if (nr > 0 && slow_passes % RAW_EVERY_SLOW_PASSES == 0 && (slow_i % SLOW_PER_CYCLE) == 0) {
                uint8_t b[32];
                char hex[80];
                uint8_t pid = raw[(raw_i++) % nr];
                if (query_pid(pid, b, sizeof b, hex, sizeof hex) > 0) sdlog_write_raw(pid, hex);
            }
            if (!s_link_up || s_halt) { sched_end(poll_job); break; }   /* every exit ends the run, or the watchdog stays subscribed */

            int64_t now = esp_timer_get_time();
            if (now - last_rv > 10 * 1000000LL) {
                if (elm_transact("ATRV", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) {
                    float v = strtof(resp, NULL);
                    if (v > 0) { STATUS_LOCK(); g_status.obd.adapter_voltage_v = v; g_status.obd.adapter_v_valid = true; STATUS_UNLOCK(); }
                }
                last_rv = now;
            }
            if (now - last_dtc > (int64_t)remote_config_get()->dtc_interval_s * 1000000LL) {
                poll_dtcs();
                last_dtc = now;
            }

            if (any_ok) {
                consecutive_fail = 0;
                last_ok = esp_timer_get_time();
                obd_values_t snap;
                STATUS_LOCK(); snap = g_status.obd; STATUS_UNLOCK();
                {
                    /* fuel and economy from the best source this car has; older cars fall back, missing stays missing */
                    ml_stats_t ms;
                    ml_get_stats(&ms);
                    int64_t t_us = esp_timer_get_time();
                    bool model_ok = ms.windows > 0 && ms.last_at_ms >= drive_start_ms && t_us / 1000 - ms.last_at_ms < 15000;
                    /* PID 0x44 is polled slowly. A commanded 0 (fuel cut) is trusted only while it is recent or the throttle has not
                       opened since it was read; otherwise a stale 0 would zero the fuel rate after the driver is back on the pedal. */
                    bool equiv_ok = OBD_HAS(&snap, F_equiv);
                    if (equiv_ok && snap.f[F_equiv] <= 0.05f) {
                        bool recent = equiv_at_us && t_us - equiv_at_us < 3000000;
                        bool thr_closed = equiv_thr >= 0 && OBD_HAS(&snap, F_throttle) && snap.f[F_throttle] <= equiv_thr + 2.0f;
                        equiv_ok = recent || thr_closed;
                    }
                    /* R.E.V (when selected): the physics and learned estimates for cars without MAF, and which one this car
                       trusts. Off with E.V: then this is exactly econ_fuel_lph(). */
                    rev_est_t re;
                    rev_estimates(&snap, &re);
                    econ_in_t in = {OBD_HAS(&snap, F_fuel_rate), snap.f[F_fuel_rate], OBD_HAS(&snap, F_maf), snap.f[F_maf],
                                    equiv_ok, snap.f[F_equiv], OBD_HAS(&snap, F_fuel_type) ? (int)snap.f[F_fuel_type] : 0,
                                    model_ok, ms.last_est[2], re.phys_ok, re.phys_maf_gps, re.learned_ok, re.learned_lph, re.prefer};
                    fuel_src_t src;
                    float raw = econ_fuel_pick(&in, &src);
                    float cal = rev_cal_factor();                 /* fill-up calibration: 1.0 until the user enters fill-ups */
                    float lph = raw >= 0 ? raw * cal : raw;
                    float kph = OBD_HAS(&snap, F_speed) ? snap.f[F_speed] : -1;
                    float dt = last_row_us ? (float)(t_us - last_row_us) / 1e6f : 0;
                    if (last_row_us) { econ_step(&trip, kph, lph, dt); econ_step(&trip_raw, kph, raw, dt); }
                    last_row_us = t_us;
                    rev_row(&snap, t_us / 1000, src, raw, dt);   /* labels, airflow constant, ledger: before ml_feed takes the label */
                    snap.fuel_lph_raw = raw;
                    snap.fuel_cal = cal;
                    snap.trip_fuel_raw_l = (float)trip_raw.litres;
                    snap.fuel_lph_calc = lph;
                    snap.fuel_src = (uint8_t)src;
                    snap.trip_km = trip.km;
                    snap.trip_fuel_l = (float)trip.litres;
                    snap.econ_trip_kpl = econ_trip_kpl(&trip);
                    snap.econ_inst_kpl = econ_instant_kpl(kph, lph);
                    STATUS_LOCK();
                    g_status.obd.fuel_lph_calc = snap.fuel_lph_calc;   g_status.obd.fuel_src = snap.fuel_src;
                    g_status.obd.trip_km = snap.trip_km;               g_status.obd.trip_fuel_l = snap.trip_fuel_l;
                    g_status.obd.econ_trip_kpl = snap.econ_trip_kpl;   g_status.obd.econ_inst_kpl = snap.econ_inst_kpl;
                    g_status.obd.fuel_lph_raw = snap.fuel_lph_raw;     g_status.obd.fuel_cal = snap.fuel_cal;
                    g_status.obd.trip_fuel_raw_l = snap.trip_fuel_raw_l;
                    STATUS_UNLOCK();
                }
                ml_feed(&snap, esp_timer_get_time() / 1000);   /* before pattern_step: the closing row is in its second */
                pattern_step(&snap);
                sdlog_write_row(&snap);
            } else if (++consecutive_fail > 10 || esp_timer_get_time() - last_ok > ECU_QUIET_US) {
                sys_event(EV_WARN, "ECU stopped answering: drive ended");
                sched_end(poll_job);
                break;                                             /* ignition off is the usual reason: end_drive() saves */
            }
            sched_end(poll_job);
            uint32_t gap = remote_config_get()->poll_interval_ms;
            if (s_resp_count && gap > 50) gap = 50;      /* replies come back at once; nothing to wait for between cycles */
            vTaskDelay(pdMS_TO_TICKS(gap));
        }
        end_drive(poll_job);                                       /* everything logged so far is on the card, files closed */
        s_in_drive = false;
        if (s_link_up && !s_halt) {
            vTaskDelay(pdMS_TO_TICKS(5000));                       /* engine off: retry calmly until the ECU answers again */
            if (s_link_up && !s_halt) xEventGroupSetBits(s_ev, LINK_UP_BIT);
        }
    }
}

void elm_init(void)
{
    s_rx = xStreamBufferCreate(2048, 1);
    s_ev = xEventGroupCreate();
    s_txn_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(obd_task, "obd", 8192, NULL, 5, NULL, 1);
}
