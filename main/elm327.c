#include <string.h>
#include <math.h>
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
#include "obd_fresh.h"
#include "status.h"
#include "sdlog.h"
#include "custom_pids.h"
#include "fault_engine.h"
#include "pattern.h"
#include "events.h"
#include "remote_config.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "ecu_select.h"
#if BOARD_MOSAICO
#include "ids_svc.h"                   /* the network guard's link head: the round-trip hook (a counter, no more) */
#endif

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

/* ESP-Mosaico field fix (2.1.1): the helpers that run once per connection keep their buffers off the obd task's stack
   while it polls. Inlined into obd_task they made its frame 5,104 B of the 8,192 B stack, and the first row of a boot then
   called pattern_model_from_json (4,016 B more): the stack overflow in the field core dump. tools/stack_check.py checks it. */
#define OBD_STACK 8192
#if BOARD_MOSAICO
#define OBD_COLD __attribute__((noinline, cold))
#else
#define OBD_COLD
#endif
/* the attribute goes on these declarations, so the definitions below read as before */
OBD_COLD static void pattern_load(void);
OBD_COLD static void pattern_save(void);
OBD_COLD static void read_supported(void);
OBD_COLD static void probe_resp_count(const char *protocol);
OBD_COLD static void poll_dtcs(void);
OBD_COLD static bool try_protocols(char *resp, size_t sz);
OBD_COLD static void pick_engine_ecu(void);
OBD_COLD static bool elm_initialise(void);
OBD_COLD static void end_drive(sched_job_t poll_job);
OBD_COLD static void run_explore(void);

static void pattern_load(void)
{
    EXT_RAM_BSS_ATTR static char buf[12288];
    pattern_init(&s_pattern);
#if BOARD_MOSAICO
    /* parsed in place (PSRAM): pattern_model_from_json keeps a 3.5 KB copy of the model on the caller's stack */
    if (sdlog_load_text(PATTERNS_PATH, buf, sizeof buf) && pattern_model_load_json(&s_pattern, buf))
#else
    if (sdlog_load_text(PATTERNS_PATH, buf, sizeof buf) && pattern_model_from_json(&s_pattern, buf))
#endif
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
#if BOARD_MOSAICO
    if (sched_safe_mode()) return;                       /* boot-loop safe mode: the pattern layer stays off too */
#endif
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
static char         s_poll_header[12];      /* the main poll's header: the engine's physical address when several computers
                                               answer on CAN (pick_engine_ecu), else s_default_header */

/* ------------------------------------------------------------------ connection search state */
static volatile bool     s_explore_req, s_exploring;
static char              s_explore_hdr[12];   /* engine address found by the last search, "" = none */
static char              s_explore_proto;     /* protocol digit that answered when auto search did not, 0 = auto */
EXT_RAM_BSS_ATTR static char s_explore_json[256];
static volatile uint32_t s_link_epoch;
static volatile int64_t  s_link_up_ms;

static void show_prompt(const char *text, uint32_t ms)
{
    STATUS_LOCK();
    snprintf(g_status.prompt_text, sizeof g_status.prompt_text, "%s", text);
    g_status.prompt_until_ms = esp_timer_get_time() / 1000 + ms;
    STATUS_UNLOCK();
}

/* JSON-safe copy of an adapter reply: printable ASCII only, quotes and backslashes dropped */
static void json_clean(char *dst, size_t n, const char *src)
{
    size_t k = 0;
    for (; src && *src && k + 1 < n; src++)
        if (*src >= 0x20 && *src < 0x7F && *src != '"' && *src != '\\') dst[k++] = *src;
    dst[k] = 0;
}

/* ------------------------------------------------------------------ transport glue */
void elm_rx_bytes(const uint8_t *data, size_t len)
{
    if (s_rx) xStreamBufferSend(s_rx, data, len, 0);
}

void elm_link_changed(bool up)
{
    if (up && !s_link_up) { s_link_epoch++; s_link_up_ms = esp_timer_get_time() / 1000; }
    if (!up) s_explore_req = false;                   /* a search asked for on a link that dropped is void */
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
#if BOARD_MOSAICO
    if (cmd[0] == '0' && cmd[1] == '1')                 /* mode 01 requests only: their round trip is the link head's input */
        IDS_HOOK(ids_hook_link_rtt((uint32_t)(esp_timer_get_time() - (deadline - (int64_t)timeout_ms * 1000))));
#endif
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

/* Freshness (obd_fresh.h): when each field was last read, which fields are in the fast tier, and the typical cycle. obd task only. */
static uint32_t s_field_ms[F_COUNT];
static uint64_t s_fast_fields[2];
static uint32_t s_cycle_ms = 1500;
static uint32_t s_level_seen_ms;                    /* fuel-level read time last given to the tank learner */

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
        s_field_ms[i] = (uint32_t)(esp_timer_get_time() / 1000);
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
    } else if (strcmp(s_cur_header, s_poll_header) != 0) {
        set_header(s_poll_header);
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

/* On CAN, more than one computer can answer a broadcast request (engine and transmission, hybrid systems), in either
   order, and the poll reads the first reply. Once per connect, ask 0100 with headers on, pick the engine
   (ecu_pick_engine) and address it physically, so every reply is the engine's. Verified with 0100 (the same list) and
   010C (RPM) on the new address, or the poll stays on the broadcast address as before. One computer answering changes
   nothing. A one-shot scheduler job: watched while it runs, not expected again until the next connect. */
static void pick_engine_ecu(void)
{
    snprintf(s_poll_header, sizeof s_poll_header, "%s", s_default_header);
    if (!s_default_header[0]) return;                 /* not CAN: one answer per request */
    static sched_job_t job = -1;
    if (job < 0) job = sched_register("ecu_pick", 10000, 10000, true);
    sched_set_expected(job, true);
    sched_begin(job);

    bool ext29 = strlen(s_default_header) == 8;
    char resp[256], hex[64];
    if (s_explore_hdr[0]) {                           /* the last search found the engine here: check it still answers */
        set_header(s_explore_hdr);
        bool ok = strcmp(s_cur_header, s_explore_hdr) == 0 &&
                  elm_transact("010C", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0 && !resp_is_error(resp);
        if (ok) { collapse_hex(resp, hex, sizeof hex); ok = strncmp(hex, "410C", 4) == 0 && strlen(hex) >= 8; }
        if (ok) {
            snprintf(s_poll_header, sizeof s_poll_header, "%s", s_explore_hdr);
            char j[96];
            snprintf(j, sizeof j, "{\"result\":\"search\",\"poll_header\":\"%s\"}", s_poll_header);
            sdlog_meta_set_json("poll_ecu", j);
            sys_event(EV_INFO, "Polling %s (found by search)", s_poll_header);
            sched_end(job);
            sched_set_expected(job, false);
            return;
        }
        set_header(s_default_header);
    }
    ecu_t ecus[8];
    int n = 0;
    set_header(s_default_header);
    if (elm_transact("ATH1", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0 && strstr(resp, "OK")) {
        sched_kick();
        if (elm_transact("0100", resp, sizeof resp, 3000) > 0) n = ecu_parse_replies(resp, ext29, ecus, 8);
        sched_kick();
        elm_transact("ATH0", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS);
    }
    int pick = n > 1 ? ecu_pick_engine(ecus, n) : -1;
    const char *result = n <= 1 ? "single" : pick < 0 ? "no-rpm" : "fallback";
    if (pick >= 0) {
        sched_kick();
        set_header(ecus[pick].req_hdr);
        char want[16];
        snprintf(want, sizeof want, "4100%02X%02X%02X%02X", ecus[pick].sup01[0], ecus[pick].sup01[1], ecus[pick].sup01[2], ecus[pick].sup01[3]);
        bool ok = strcmp(s_cur_header, ecus[pick].req_hdr) == 0 &&
                  elm_transact("0100", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0 && !resp_is_error(resp);
        if (ok) { collapse_hex(resp, hex, sizeof hex); ok = strcmp(hex, want) == 0; }
        if (ok) {
            sched_kick();
            ok = elm_transact("010C", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0 && !resp_is_error(resp);
            if (ok) { collapse_hex(resp, hex, sizeof hex); ok = strncmp(hex, "410C", 4) == 0 && strlen(hex) >= 8; }
        }
        if (ok) {
            snprintf(s_poll_header, sizeof s_poll_header, "%s", ecus[pick].req_hdr);
            result = "engine";
            sys_event(EV_INFO, "Engine ECU %s picked (%d answering)", ecus[pick].resp_id, n);
        } else {
            set_header(s_default_header);
            sys_event(EV_WARN, "Engine ECU %s did not verify: polling all ECUs", ecus[pick].resp_id);
        }
    }
    char j[160];
    snprintf(j, sizeof j, "{\"result\":\"%s\",\"answering\":%d,\"engine\":\"%s\",\"poll_header\":\"%s\"}",
             result, n, pick >= 0 ? ecus[pick].resp_id : "", s_poll_header);
    sdlog_meta_set_json("poll_ecu", j);
    sched_end(job);
    sched_set_expected(job, false);
}

static bool elm_initialise(void)
{
    char resp[256];
    int rc;
    STATUS_LOCK(); g_status.ble = BLE_ST_ELM_INIT; STATUS_UNLOCK();
    s_cur_header[0] = 0;
    s_default_header[0] = 0;
    s_poll_header[0] = 0;

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
#if CONFIG_OBD_BLE_PAIR_CONFIRM
    /* it answered with a prompt: an ELM327-style adapter. A new one is not used before the user says yes (obd_pair.h) */
    if (!obd_ble_adapter_confirmed()) return false;
#else
    obd_ble_adapter_confirmed();           /* it answered with a prompt: an ELM327-style adapter, keep it */
#endif
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
    if (s_explore_proto) {                 /* auto search failed on this car before; the search found this protocol */
        char cmd[8];
        snprintf(cmd, sizeof cmd, "ATTP%c", s_explore_proto);
        elm_transact(cmd, resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS);
    }
    if (elm_transact("ATI", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) snprintf(ati, sizeof ati, "%.31s", resp);
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
    {   /* pairing safety (adapter_guard.h R1): the device description too, then the paired adapter's fingerprint check */
        char at1[48] = "";
        if (elm_transact("AT@1", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) snprintf(at1, sizeof at1, "%.47s", resp);
        obd_ble_guard_elm(ati, at1);
    }
#endif
#if BOARD_MOSAICO
    /* only the adapter's name and address: a copy of the whole status (status_get) is 3.6 KB of the obd task's stack */
    char st_name[sizeof g_status.adapter_name], st_addr[sizeof g_status.adapter_addr];
    STATUS_LOCK();
    memcpy(st_name, g_status.adapter_name, sizeof st_name);
    memcpy(st_addr, g_status.adapter_addr, sizeof st_addr);
    STATUS_UNLOCK();
    st_name[sizeof st_name - 1] = 0;
    st_addr[sizeof st_addr - 1] = 0;
#else
    app_status_t st;
    status_get(&st);
#endif

    int tries = 0;
    do {
        rc = elm_transact("0100", resp, sizeof resp, 12000);
        if (rc > 0 && strstr(resp, "4100")) break;
        ESP_LOGW(TAG, "0100 -> '%s' (ignition off?)", resp);
        /* auto search gave up although the adapter answers: try each protocol in turn, as python-OBD does */
        if (rc > 0 && (tries == 2 || tries == 10) && (strstr(resp, "UNABLE") || strstr(resp, "ERROR")) &&
            try_protocols(resp, sizeof resp)) break;
        vTaskDelay(pdMS_TO_TICKS(3000));
    } while (++tries < 20 && s_link_up && !s_halt && !s_explore_req);
    if (s_explore_req) return false;                                   /* a connection search was asked for */
    if (!s_link_up || s_halt || tries >= 20) { if (s_link_up && !s_halt) sys_event(EV_WARN, "ECU not answering: ignition off?"); return false; }

    sdlog_begin_drive();                   /* the ECU answered: a new session if the last drive was closed */
#if BOARD_MOSAICO
    sdlog_meta_set_adapter(st_name, st_addr, ati, batch);
#else
    sdlog_meta_set_adapter(st.adapter_name, st.adapter_addr, ati, batch);
#endif
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
    obd_ble_guard_meta();                  /* the session says whether this adapter matched its fingerprint */
#endif
    if (s_explore_json[0]) sdlog_meta_set_json("search", s_explore_json);

    char protocol[24] = "";
    if (elm_transact("ATDPN", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) snprintf(protocol, sizeof protocol, "%.23s", resp);
    {
        char pn = protocol[0] == 'A' ? protocol[1] : protocol[0];   /* "A6" = auto-detected protocol 6 */
        if (pn == '6' || pn == '8') strcpy(s_default_header, "7DF");
        else if (pn == '7' || pn == '9') strcpy(s_default_header, "18DB33F1");
    }
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
    obd_ble_guard_protocol(protocol);
#endif

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

    pick_engine_ecu();                     /* before read_supported: the PID list must be the engine's */
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
#if BOARD_MOSAICO
    /* the field core dump was a stack overflow in this task: every drive reports how much of its stack was never used */
    unsigned free_b = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "obd stack: %u B of %u B never used this boot", free_b, OBD_STACK);
    if (free_b < 1024) sys_event(EV_WARN, "OBD task stack low: %u B left", free_b);
#endif
}

void elm_request_stop(void) { s_halt = true; }

bool elm_wait_stopped(uint32_t timeout_ms)
{
    s_halt = true;
    for (uint32_t t = 0; s_in_drive && t < timeout_ms; t += 50) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_in_drive) sdlog_end_drive();    /* also covers a stop before any drive started */
    return !s_in_drive;
}

#if BOARD_MOSAICO
/* the user's stop (stop_core.h): held until elm_user_stop(false), whatever else asks to resume */
static volatile bool s_user_stop;

void elm_user_stop(bool on)
{
    s_user_stop = on;
    s_halt = on;
    if (!on && s_link_up) xEventGroupSetBits(s_ev, LINK_UP_BIT);
}

bool elm_user_stopped(void) { return s_user_stop; }
#endif

void elm_resume(void)
{
#if BOARD_MOSAICO
    if (s_user_stop) return;           /* stopped by the user: only Start logging (or a restart) brings the link back */
#endif
    s_halt = false;
    if (s_link_up) xEventGroupSetBits(s_ev, LINK_UP_BIT);
}

/* The connection search (elm_explore): bounded, read-only, about a minute at most. Every step is an ELM transaction with
   its own timeout and the watchdog is fed after each, so a silent car ends the search instead of hanging it. */
static void run_explore(void)
{
    s_explore_req = false;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 4096) {   /* never start optional work short of RAM */
        show_prompt("Search postponed: memory low. Try again soon.", 10000);
        return;
    }
    s_exploring = true;
    static sched_job_t job = -1;
    if (job < 0) job = sched_register("link_search", 90000, 90000, true);
    sched_set_expected(job, true);
    sched_begin(job);
    char resp[256], hex[64], text[64], dpn[12] = "";
    ecu_t ecus[8];
    bool rpm_ok[8] = {0};
    int n = 0, pick = -1;

    s_cur_header[0] = 0;
    elm_transact("ATZ", resp, sizeof resp, 4000);
    sched_kick();
    static const char *init[] = {"ATE0", "ATL0", "ATS0", "ATH0", "ATAT1", "ATST32"};
    for (size_t i = 0; i < sizeof init / sizeof init[0]; i++) { elm_transact(init[i], resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS); sched_kick(); }

    /* 1. protocol: auto search first, then each protocol on its own (python-OBD order, CAN first) */
    static const char order[] = "0681792345";
    char proto = 0;
    for (const char *p = order; *p && s_link_up && !s_halt; p++) {
        char cmd[8];
        if (*p == '0') snprintf(cmd, sizeof cmd, "ATSP0");
        else snprintf(cmd, sizeof cmd, "ATTP%c", *p);
        if (elm_transact(cmd, resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) < 0) break;
        if (*p != '0' && strchr(resp, '?')) { snprintf(cmd, sizeof cmd, "ATSP%c", *p); elm_transact(cmd, resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS); }
        snprintf(text, sizeof text, "Searching: protocol %s...", *p == '0' ? "auto" : cmd + 4);
        show_prompt(text, 30000);
        sched_kick();
        int rc = elm_transact("0100", resp, sizeof resp, *p == '0' ? 12000 : 6000);
        sched_kick();
        if (rc > 0 && strstr(resp, "4100")) { proto = *p; break; }
    }
    if (!proto) {
        s_explore_hdr[0] = 0;
        s_explore_proto = 0;
        snprintf(text, sizeof text, "No computer answered on any protocol. Ignition on?");
    } else {
        if (elm_transact("ATDPN", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) json_clean(dpn, sizeof dpn, resp);
        s_explore_proto = proto == '0' ? 0 : proto;
        char pn = dpn[0] == 'A' ? dpn[1] : dpn[0];
        if (pn >= '6' && pn <= '9') {
            /* 2. computers: the broadcast request with headers on, then each 11-bit physical address it missed */
            bool ext29 = pn == '7' || pn == '9';
            const char *func = ext29 ? "18DB33F1" : "7DF";
            set_header(func);
            if (elm_transact("ATH1", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS) > 0) {
                sched_kick();
                if (elm_transact("0100", resp, sizeof resp, 3000) > 0) n = ecu_parse_replies(resp, ext29, ecus, 8);
                sched_kick();
                elm_transact("ATH0", resp, sizeof resp, CONFIG_OBD_ELM_TIMEOUT_MS);
            }
            for (int i = 0; !ext29 && i < 8 && n < 8 && s_link_up; i++) {
                char rid[4], req[4];
                snprintf(rid, sizeof rid, "%03X", 0x7E8 + i);
                snprintf(req, sizeof req, "%03X", 0x7E0 + i);
                bool known = false;
                for (int k = 0; k < n; k++) known |= strcmp(ecus[k].resp_id, rid) == 0;
                if (known) continue;
                set_header(req);
                uint8_t b[4];
                if (strcmp(s_cur_header, req) == 0 && elm_transact("0100", resp, sizeof resp, 1000) > 0 && !resp_is_error(resp) &&
                    bytes_after_prefix(resp, "4100", b, sizeof b, NULL, 0) == 4) {
                    memset(&ecus[n], 0, sizeof ecus[n]);
                    snprintf(ecus[n].resp_id, sizeof ecus[n].resp_id, "%s", rid);
                    snprintf(ecus[n].req_hdr, sizeof ecus[n].req_hdr, "%s", req);
                    memcpy(ecus[n].sup01, b, 4);
                    n++;
                }
                sched_kick();
            }
            /* 3. which of them answers RPM on its own address; the engine among those (ecu_pick_engine) */
            ecu_t ok_e[8];
            int map[8], m = 0;
            for (int i = 0; i < n && s_link_up; i++) {
                set_header(ecus[i].req_hdr);
                if (strcmp(s_cur_header, ecus[i].req_hdr) == 0 && elm_transact("010C", resp, sizeof resp, 1500) > 0 && !resp_is_error(resp)) {
                    collapse_hex(resp, hex, sizeof hex);
                    rpm_ok[i] = strncmp(hex, "410C", 4) == 0 && strlen(hex) >= 8;
                }
                if (rpm_ok[i]) { ok_e[m] = ecus[i]; map[m++] = i; }
                sched_kick();
            }
            int pk = m ? ecu_pick_engine(ok_e, m) : -1;
            pick = pk >= 0 ? map[pk] : -1;
            snprintf(s_explore_hdr, sizeof s_explore_hdr, "%s", pick >= 0 ? ecus[pick].req_hdr : "");
            set_header(func);
        } else s_explore_hdr[0] = 0;          /* J1850 / K-line: physical headers are not used, stay on broadcast */
        ecu_search_summary(dpn, ecus, rpm_ok, n, pick, text, sizeof text);
    }
    char clean[64];
    json_clean(clean, sizeof clean, text);
    snprintf(s_explore_json, sizeof s_explore_json, "{\"protocol\":\"%s\",\"forced\":\"%c\",\"computers\":%d,\"engine\":\"%s\",\"summary\":\"%s\"}",
             dpn, s_explore_proto ? s_explore_proto : '-', n, s_explore_hdr, clean);
    show_prompt(text, 20000);
    sys_event(EV_INFO, "Search: %.46s", text);
    sched_end(job);
    sched_set_expected(job, false);
    s_exploring = false;
}

static void obd_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(s_ev, LINK_UP_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s_halt) continue;                                     /* stopped for power-off: wait for elm_resume() */
        if (s_explore_req && s_link_up) run_explore();

        if (!elm_initialise()) {
            if (s_link_up) {
                vTaskDelay(pdMS_TO_TICKS(s_explore_req ? 10 : 5000));
                if (s_link_up) xEventGroupSetBits(s_ev, LINK_UP_BIT);
            }
            continue;
        }

        s_in_drive = true;                                /* from here the model must not change (usb_door refuses MODEL SET) */
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
        s_fast_fields[0] = s_fast_fields[1] = 0;
        for (int i = 0; i < F_COUNT; i++)
            for (int k = 0; k < nf; k++)
                if (FIELDS[i].pid == fast[k]) { s_fast_fields[i / 64] |= 1ULL << (i % 64); break; }
        s_cycle_ms = 1500;
        int64_t last_cycle_us = 0;

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
        g_status.obd.torque_nm = NAN; g_status.obd.torque_src = 0; g_status.obd.tank_left_l = -1; g_status.obd.tank_range_km = -1;
        STATUS_UNLOCK();
        rev_drive_started();
        sdlog_meta_set_ml(ml_model_key(ml_model_active()), rev_cal_factor());
        char resp[64];
        s_in_drive = true;

        while (s_link_up && !s_halt && !s_explore_req) {
            sched_begin(poll_job);
            if (strcmp(s_cur_header, s_poll_header) != 0) set_header(s_poll_header);   /* the DTC read below goes to every computer */
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
            if (strcmp(s_cur_header, s_poll_header) != 0) set_header(s_poll_header);
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
                if (strcmp(s_cur_header, s_default_header) != 0) set_header(s_default_header);   /* codes from every computer */
                poll_dtcs();
                last_dtc = now;
            }

            if (any_ok) {
                consecutive_fail = 0;
                last_ok = esp_timer_get_time();
                if (last_cycle_us) {                        /* typical cycle, for the freshness limits */
                    int64_t c = (last_ok - last_cycle_us) / 1000;
                    if (c > 0 && c < 60000) s_cycle_ms = (uint32_t)(0.8f * (float)s_cycle_ms + 0.2f * (float)c);
                }
                last_cycle_us = last_ok;
                obd_values_t snap;
                /* a field that stopped answering is missing, not frozen at its last value: the fuel picker, R.E.V and the model
                   then see the gap (a stale MAF kept the picker on MAF and fed R.E.V a frozen label before) */
                uint32_t slow_cycles = (uint32_t)((total_slow + SLOW_PER_CYCLE - 1) / SLOW_PER_CYCLE);
                STATUS_LOCK();
                int gone = obd_fresh_expire(g_status.obd.valid, s_fast_fields, s_field_ms, F_COUNT, (uint32_t)(last_ok / 1000),
                                            s_cycle_ms, slow_cycles);
                snap = g_status.obd;
                STATUS_UNLOCK();
                if (gone) ESP_LOGD(TAG, "%d field(s) stopped answering", gone);
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
                    /* a fresh fuel-level answer this row (not one carried over), for the tank learner */
                    bool level_fresh = OBD_HAS(&snap, F_fuel_level) && s_field_ms[F_fuel_level] != s_level_seen_ms;
                    if (level_fresh) s_level_seen_ms = s_field_ms[F_fuel_level];
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
                    if (!obd_ble_adapter_changed())   /* pairing safety: a different-looking adapter teaches the learner nothing */
#endif
                    rev_row(&snap, t_us / 1000, src, raw, dt, level_fresh);   /* labels, airflow constant, ledger: before ml_feed takes the label */
                    bool tq_idle;
                    rev_row_outputs(&snap.torque_nm, &snap.torque_src, &tq_idle, &snap.tank_left_l, &snap.tank_range_km);
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
                    g_status.obd.torque_nm = snap.torque_nm;           g_status.obd.torque_src = snap.torque_src;
                    g_status.obd.tank_left_l = snap.tank_left_l;       g_status.obd.tank_range_km = snap.tank_range_km;
                    STATUS_UNLOCK();
                }
                ml_feed(&snap, esp_timer_get_time() / 1000);   /* before pattern_step: the closing row is in its second */
#if CONFIG_OBD_BLE_PAIR_CONFIRM && BOARD_MOSAICO
                obd_ble_guard_meta();
                if (!obd_ble_adapter_changed())                /* the pattern model learns from rows too */
#endif
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
            vTaskDelay(pdMS_TO_TICKS(s_explore_req ? 10 : 5000));  /* engine off: retry calmly until the ECU answers again */
            if (s_link_up && !s_halt) xEventGroupSetBits(s_ev, LINK_UP_BIT);
        }
    }
}

bool elm_drive_active(void) { return s_in_drive; }

#if CONFIG_OBD_BLE_PAIR_CONFIRM
static void explore(bool user)
{
    if (s_exploring) return;
    if (!s_link_up) { obd_ble_explore(user); show_prompt("Searching for an OBD adapter...", 15000); return; }
    s_explore_req = true;
    show_prompt("Searching protocols and computers...", 60000);
}

void elm_explore(void) { explore(true); }
void elm_explore_auto(void) { explore(false); }
#else
void elm_explore(void)
{
    if (s_exploring) return;
    if (!s_link_up) { obd_ble_explore(); show_prompt("Searching for an OBD adapter...", 15000); return; }
    s_explore_req = true;
    show_prompt("Searching protocols and computers...", 60000);
}
#endif

bool elm_exploring(void) { return s_exploring || s_explore_req; }
bool elm_link_is_up(void) { return s_link_up; }
int64_t elm_link_up_ms(void) { return s_link_up ? s_link_up_ms : -1; }
uint32_t elm_link_epoch(void) { return s_link_epoch; }

void elm_link_summary(char *out, size_t n)
{
    snprintf(out, n, "Link OK: polling %s", s_poll_header[0] ? s_poll_header : "all computers");
}

void elm_init(void)
{
    s_rx = xStreamBufferCreate(2048, 1);
    s_ev = xEventGroupCreate();
    s_txn_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(obd_task, "obd", OBD_STACK, NULL, 5, NULL, 1);
}
