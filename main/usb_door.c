#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "psa/crypto.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "usb_door.h"
#include "uploader.h"
#include "elm327.h"
#include "wifi_creds.h"
#include "remote_config.h"
#include "sdlog.h"
#include "status.h"
#include "sys_mode.h"
#include "version.h"
#include "ml_infer.h"
#include "rev_task.h"

static const char *TAG = "door";

#define LINE_CAP        2600
#define GET_MAX         3072
#define SESSION_IDLE_MS (10 * 60 * 1000)
#define CODE_MS         120000
#define CODE_TRIES      3

static bool    s_authed;
static int64_t s_last_ms;
static char    s_nonce[33];
static char    s_code[7];
static int64_t s_code_until, s_code_made;
static int     s_code_tries;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* The driver refuses a write larger than its 1 KB transmit buffer (it returns 0), so send in pieces. */
static void write_all(const char *s, size_t n)
{
    while (n) {
        size_t piece = n < 256 ? n : 256;
        int w = usb_serial_jtag_write_bytes(s, piece, pdMS_TO_TICKS(500));
        if (w <= 0) return;                         /* nobody is reading: drop it */
        s += w; n -= (size_t)w;
    }
}

static void reply(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + 6, sizeof line - 8, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 9) n = sizeof line - 9;
    memcpy(line, "\nOCM1 ", 6);                      /* start on a fresh line even if a log line was half written */
    line[6 + n] = '\n';
    write_all(line, (size_t)n + 7);
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", in[i]);
}

static bool ct_equal(const char *a, const char *b, size_t n)
{
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* HMAC-SHA256 through PSA, the public crypto interface in ESP-IDF 6 (IDF initialises PSA at start-up). */
static bool hmac_hex(const char *key, const char *msg, char out[65])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, strlen(key) * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id;
    if (psa_import_key(&attr, (const uint8_t *)key, strlen(key), &id) != PSA_SUCCESS) return false;
    uint8_t mac[32];
    size_t len = 0;
    psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), (const uint8_t *)msg, strlen(msg), mac, sizeof mac, &len);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || len != sizeof mac) return false;
    hex(mac, sizeof mac, out);
    return true;
}

/* Base64 text to a NUL-terminated buffer of at most cap-1 bytes. */
static bool unb64(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (!in || mbedtls_base64_decode((unsigned char *)out, cap - 1, &n, (const unsigned char *)in, strlen(in)) != 0) return false;
    out[n] = 0;
    return strlen(out) == n;                         /* no embedded NUL */
}

static bool valid_name(const char *n)
{
    if (strlen(n) < 11 || n[0] != 's') return false;
    for (int i = 1; i <= 6; i++) if (!isdigit((unsigned char)n[i])) return false;
    const char *e = n + 7;
    return !strcmp(e, ".csv") || !strcmp(e, ".ml.csv") || !strcmp(e, ".pat") || !strcmp(e, ".raw") || !strcmp(e, ".json");
}

static void show_code(bool on)
{
    STATUS_LOCK();
    if (on) {
        snprintf(g_status.sys_popup, sizeof g_status.sys_popup, "Desktop pairing code  %s", s_code);
        g_status.sys_popup_until_ms = s_code_until;
        g_status.sys_popup_error = false;
    } else if (strncmp(g_status.sys_popup, "Desktop pairing code", 20) == 0) {
        g_status.sys_popup[0] = 0;
    }
    STATUS_UNLOCK();
}

static void cmd_get(char *name, char *off_s, char *len_s, char *buf, char *b64)
{
    long off = off_s ? atol(off_s) : -1, len = len_s ? atol(len_s) : -1;
    if (!name || !valid_name(name) || off < 0 || len <= 0 || len > GET_MAX) { reply("ERR usage: GET <name> <offset> <1..%d>", GET_MAX); return; }
    char path[96];
    snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%.20s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { reply("ERR no such file"); return; }
    size_t n = fseek(f, off, SEEK_SET) == 0 ? fread(buf, 1, (size_t)len, f) : 0;
    fclose(f);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)b64, 4 * ((GET_MAX + 2) / 3) + 1, &olen, (const unsigned char *)buf, n);
    write_all("\nOCM1 D ", 8);
    write_all(b64, olen);
    write_all("\n", 1);
    reply("OK GET %u", (unsigned)n);
}

/* OK MODEL active=<key> available=<keys> loaded=<0|1> learning=<0|1> run_us=<one timed run at load> */
static void reply_model(void)
{
    ml_stats_t ms;
    ml_get_stats(&ms);
    char avail[16] = "";
    for (int i = 0; i < ML_MODEL_COUNT; i++)
        if (ml_model_available(i)) snprintf(avail + strlen(avail), sizeof avail - strlen(avail), "%s%s", avail[0] ? "," : "", ml_model_key(i));
    reply("OK MODEL active=%s available=%s loaded=%d learning=%d run_us=%lu", ml_model_key(ml_model_active()), avail[0] ? avail : "none",
          ms.ready ? 1 : 0, ml_rev_active() ? 1 : 0, (unsigned long)ms.selftest_us);
}

static void cmd_model(const char *sub, const char *key)
{
    if (sub && !strcmp(sub, "GET")) { reply_model(); return; }
    if (!sub || strcmp(sub, "SET") || !key) { reply("ERR usage: MODEL GET | MODEL SET ev|rev"); return; }
    if (elm_drive_active()) { reply("ERR a drive is running: switch models while parked"); return; }
    int id = -1;
    for (int i = 0; i < ML_MODEL_COUNT; i++) if (!strcmp(key, ml_model_key(i))) id = i;
    if (id < 0) { reply("ERR unknown model %.8s: ev or rev", key); return; }
    char why[80];
    if (!ml_select_model(id, why, sizeof why)) { reply("ERR %s", why); return; }
    reply_model();
}

/* Several "D <group> key=value ..." lines, each well under the 190-character reply limit, then OK REV-STATUS. */
static void cmd_rev_status(void)
{
    rev_status_t st;
    rev_get_status(&st);
    static const char *const src[] = {"model", "learned", "phys"};
    reply("D state model=%s enabled=%d learning=%d safe=%d sd=%d saved=%d saves=%lu row_ms=%.0f", ml_model_key(ml_model_active()),
          st.enabled, st.learning, st.safe, st.sd, st.saved, (unsigned long)st.saves, (double)st.row_ms);
    reply("D fuel commits=%lu resets=%lu kept=%lu failed=%lu learned=%lu pairs=%lu dropped=%lu rejected=%lu drift=%.3f mse_live=%.4f mse_factory=%.4f",
          (unsigned long)st.commits, (unsigned long)st.resets, (unsigned long)st.kept, (unsigned long)st.failed, (unsigned long)st.learned,
          (unsigned long)st.pairs, (unsigned long)st.dropped, (unsigned long)st.rejected, (double)st.drift, (double)st.mse_live, (double)st.mse_prior);
    reply("D error factory=%.4f learned=%.4f phys=%.4f n_factory=%lu n_learned=%lu n_phys=%lu prefer=%s", (double)st.err[0], (double)st.err[1],
          (double)st.err[2], (unsigned long)st.n_err[0], (unsigned long)st.n_err[1], (unsigned long)st.n_err[2], src[st.prefer]);
    reply("D phys k=%.4e ready=%d rows=%lu path=%s", (double)st.k, st.k_ready, (unsigned long)st.k_rows, st.k_calc ? "calc" : "abs");
    reply("D cal c=%.4f fill_rev=%lu used=%d skipped=%d fills=%d", (double)st.cal, (unsigned long)st.fill_rev, st.used, st.skipped, st.fills);
    reply("D life litres=%.3f km=%.3f", st.life_l, st.life_km);
    {
        static const char *const tsrc[] = {"none", "pid", "rate", "maf", "k"};
        reply("D torque ok=%d nm=%.1f src=%s idle=%d vd_l=%.2f", st.torque_ok, st.torque_ok ? (double)st.torque_nm : 0.0,
              tsrc[st.torque_src < 5 ? st.torque_src : 0], st.torque_idle, (double)st.vd_l);
        const rev_tank_out_t *t = &st.tank;
        reply("D tank have=%d ready=%d cap_raw_l=%.2f rel_se=%.4f band_l=%.2f span_pct=%.1f level=%.1f left_raw_l=%.2f range_km=%.0f segs=%lu fills=%lu",
              t->have, t->ready, (double)t->cap_raw_l, (double)t->rel_se, (double)t->band_l, (double)t->span_pct, (double)t->level,
              (double)t->left_raw_l, (double)t->range_km, (unsigned long)st.tank_segs, (unsigned long)st.tank_fills);
        for (uint32_t i = 0; i < st.tank_sug_n && i < REV_TANK_SUGGEST; i++) {
            const rev_tank_fill_t *f = &st.tank_sug[i];
            reply("D tankfill seq=%lu session=%lu before=%.1f after=%.1f raw_l=%.2f ready=%d top=%d", (unsigned long)f->seq,
                  (unsigned long)f->session, (double)f->before, (double)f->after, (double)f->raw_l, f->ready, f->top);
        }
    }
    rev_interval_t iv;
    for (int i = 0; rev_get_interval(i, &iv); i++)
        reply("D iv id=%lu from=%lu to=%lu pumped=%.3f board=%.3f trip=%d why=%d", (unsigned long)iv.fill_id, (unsigned long)iv.from_session,
              (unsigned long)iv.to_session, (double)iv.pumped_l, (double)iv.raw_l, iv.trip, iv.why);
    reply("OK REV-STATUS");
}

/* {"rev": n, "entries": [{"id": 1, "after": 12, "litres": 40.1, "full": true}, {"id": 2, "kind": "trip", "from": 3, "after": 12,
   "l100": 7.9}]}: litres and L/100 km, the app converts gallons and mpg. */
static void cmd_rev_fillups(char *buf)
{
    cJSON *root = cJSON_Parse(buf);
    cJSON *rev = cJSON_GetObjectItem(root, "rev"), *arr = cJSON_GetObjectItem(root, "entries");
    static rev_fill_t f[REV_CAL_FILLUPS];
    int n = 0;
    bool ok = cJSON_IsNumber(rev) && rev->valuedouble >= 0 && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) <= REV_CAL_FILLUPS;
    cJSON *e;
    if (ok) cJSON_ArrayForEach(e, arr) {
        cJSON *id = cJSON_GetObjectItem(e, "id"), *after = cJSON_GetObjectItem(e, "after"), *kind = cJSON_GetObjectItem(e, "kind");
        bool trip = cJSON_IsString(kind) && !strcmp(kind->valuestring, "trip");
        memset(&f[n], 0, sizeof f[n]);
        if (!cJSON_IsNumber(id) || !cJSON_IsNumber(after) || after->valuedouble < 0) { ok = false; break; }
        f[n].id = (uint32_t)id->valuedouble;
        f[n].after_session = (uint32_t)after->valuedouble;
        if (trip) {
            cJSON *from = cJSON_GetObjectItem(e, "from"), *l100 = cJSON_GetObjectItem(e, "l100");
            if (!cJSON_IsNumber(from) || !cJSON_IsNumber(l100) || from->valuedouble < 0) { ok = false; break; }
            f[n].kind = REV_FILL_TRIP; f[n].from_session = (uint32_t)from->valuedouble; f[n].l_per_100km = (float)l100->valuedouble;
        } else {
            cJSON *litres = cJSON_GetObjectItem(e, "litres");
            if (!cJSON_IsNumber(litres)) { ok = false; break; }
            f[n].kind = REV_FILL_PUMP; f[n].litres = (float)litres->valuedouble; f[n].full = cJSON_IsTrue(cJSON_GetObjectItem(e, "full"));
        }
        n++;
    }
    uint32_t r = ok ? (uint32_t)rev->valuedouble : 0;
    cJSON_Delete(root);
    if (!ok) { reply("ERR usage: REV-FILLUPS <json b64> with rev and at most %d entries", REV_CAL_FILLUPS); return; }
    char why[100];
    if (!rev_set_fills(f, n, r, why, sizeof why)) { reply("ERR %s", why); return; }
    rev_status_t st;
    rev_get_status(&st);
    reply("OK REV-FILLUPS rev=%lu cal=%.4f used=%d skipped=%d", (unsigned long)st.fill_rev, (double)st.cal, st.used, st.skipped);
}

static void handle(char *line, char *buf, char *b64)
{
    if (strncmp(line, "OCM1 ", 5) != 0) return;
    char *save = NULL, *cmd = strtok_r(line + 5, " ", &save);
    char *a1 = strtok_r(NULL, " ", &save), *a2 = strtok_r(NULL, " ", &save);
    char *a3 = strtok_r(NULL, " ", &save), *a4 = strtok_r(NULL, " ", &save);
    if (!cmd) return;
    int64_t t = now_ms();

    if (!strcmp(cmd, "HELLO")) {
        uint8_t r[16];
        esp_fill_random(r, sizeof r);
        hex(r, sizeof r, s_nonce);
        s_authed = false;
        reply("OK HELLO fw=%s board=%s paired=%d nonce=%s", CARMIND_VERSION, uploader_board_id(), uploader_configured() ? 1 : 0, s_nonce);
        return;
    }
    if (!strcmp(cmd, "CODE")) {
        if (s_code_made && t - s_code_made < 5000) { reply("ERR wait a few seconds"); return; }
        snprintf(s_code, sizeof s_code, "%06lu", (unsigned long)(esp_random() % 1000000UL));
        s_code_made = t;
        s_code_until = t + CODE_MS;
        s_code_tries = CODE_TRIES;
        show_code(true);
        reply("OK CODE on the board's screen for %d s", CODE_MS / 1000);
        return;
    }
    if (!strcmp(cmd, "AUTH-CODE")) {
        if (!s_code[0] || t > s_code_until) { reply("ERR no code: send CODE first"); return; }
        if (a1 && strlen(a1) == 6 && ct_equal(a1, s_code, 6)) {
            s_code[0] = 0; show_code(false);
            s_authed = true; s_last_ms = t;
            reply("OK AUTH");
        } else if (--s_code_tries <= 0) {
            s_code[0] = 0; show_code(false);
            reply("ERR wrong code, ask for a new one");
        } else reply("ERR wrong code, %d tries left", s_code_tries);
        return;
    }
    if (!strcmp(cmd, "AUTH-HMAC")) {
        const char *token = uploader_token();
        char want[65];
        bool ok = token[0] && s_nonce[0] && a1 && strlen(a1) == 64 && hmac_hex(token, s_nonce, want) && ct_equal(a1, want, 64);
        s_nonce[0] = 0;                              /* one try per HELLO */
        if (ok) { s_authed = true; s_last_ms = t; reply("OK AUTH"); }
        else reply("ERR not accepted: send HELLO, then pair with a code if this app is new");
        return;
    }

    if (!s_authed || t - s_last_ms > SESSION_IDLE_MS) { s_authed = false; reply("ERR not authenticated"); return; }
    s_last_ms = t;
    if (sys_mode_get() == MODE_DRIVE) { reply("ERR busy: the car link is up, try again when parked"); return; }

    if (!strcmp(cmd, "PAIR")) {
        if (!a1 || !a2 || !a3 || !a4 || !unb64(a4, buf, GET_MAX)) { reply("ERR usage: PAIR <host> <port> <token> <cert b64>"); return; }
        if (uploader_pair(a1, (unsigned)atoi(a2), a3, buf)) reply("OK PAIR");
        else reply("ERR pairing values not valid");
    } else if (!strcmp(cmd, "UNPAIR")) {
        uploader_unpair();
        reply("OK UNPAIR");
    } else if (!strcmp(cmd, "WIFI-LIST")) {
        for (size_t i = 0; i < wifi_creds_count(); i++) {
            size_t olen = 0;
            const char *ssid = wifi_creds_get(i)->ssid;
            mbedtls_base64_encode((unsigned char *)b64, 64, &olen, (const unsigned char *)ssid, strlen(ssid));
            b64[olen] = 0;
            reply("D %s", b64);
        }
        reply("OK WIFI-LIST");
    } else if (!strcmp(cmd, "WIFI-ADD")) {
        char ssid[40], pw[72];
        if (!a1 || !unb64(a1, ssid, sizeof ssid) || (a2 && !unb64(a2, pw, sizeof pw))) { reply("ERR usage: WIFI-ADD <ssid b64> [password b64]"); return; }
        if (wifi_creds_add(ssid, a2 ? pw : "")) reply("OK WIFI-ADD");
        else reply("ERR rejected: SSID 1-32 bytes, password empty or 8-64 bytes, list holds 8");
    } else if (!strcmp(cmd, "WIFI-FORGET")) {
        char ssid[40];
        if (!a1 || !unb64(a1, ssid, sizeof ssid)) { reply("ERR usage: WIFI-FORGET <ssid b64>"); return; }
        if (wifi_creds_remove(ssid)) reply("OK WIFI-FORGET"); else reply("ERR no such network");
    } else if (!strcmp(cmd, "CONFIG")) {
        if (!a1 || !unb64(a1, buf, GET_MAX)) { reply("ERR usage: CONFIG <json b64>"); return; }
        cJSON *root = cJSON_Parse(buf);
        int n = cJSON_IsObject(root) ? remote_config_apply(root) : -1;
        cJSON_Delete(root);
        if (n < 0) reply("ERR not a JSON object"); else reply("OK CONFIG applied=%d", n);
    } else if (!strcmp(cmd, "LIST")) {
        DIR *d = sdlog_ok() ? opendir(SDLOG_SESSIONS_DIR) : NULL;
        if (!d) { reply("ERR no SD card"); return; }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!valid_name(e->d_name)) continue;
            char path[96];
            struct stat st;
            snprintf(path, sizeof path, SDLOG_SESSIONS_DIR "/%.20s", e->d_name);
            if (stat(path, &st) == 0) reply("D %s %ld", e->d_name, (long)st.st_size);
        }
        closedir(d);
        reply("OK LIST");
    } else if (!strcmp(cmd, "GET")) {
        cmd_get(a1, a2, a3, buf, b64);
    } else if (!strcmp(cmd, "MODEL")) {
        cmd_model(a1, a2);
    } else if (!strcmp(cmd, "REV-STATUS")) {
        cmd_rev_status();
    } else if (!strcmp(cmd, "REV-FILLUPS")) {
        if (!a1 || !unb64(a1, buf, GET_MAX)) { reply("ERR usage: REV-FILLUPS <json b64>"); return; }
        cmd_rev_fillups(buf);
    } else if (!strcmp(cmd, "REV-RESET")) {
        if (a1 && rev_reset(a1)) reply("OK REV-RESET %s", a1);
        else reply("ERR usage: REV-RESET fuel|phys|cal|tank|all (not in safe mode)");
    } else if (!strcmp(cmd, "REV-TANK-ACK")) {
        if (!a1) { reply("ERR usage: REV-TANK-ACK <seq>"); return; }
        rev_tank_suggestions_ack((uint32_t)strtoul(a1, NULL, 10));
        reply("OK REV-TANK-ACK");
    } else if (!strcmp(cmd, "REBOOT")) {
        reply("OK REBOOT");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else reply("ERR unknown command");
}

static void door_task(void *arg)
{
    (void)arg;
    char *line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM);
    char *buf = heap_caps_malloc(GET_MAX + 1, MALLOC_CAP_SPIRAM);
    char *b64 = heap_caps_malloc(4 * ((GET_MAX + 2) / 3) + 2, MALLOC_CAP_SPIRAM);
    if (!line || !buf || !b64) { ESP_LOGE(TAG, "no memory"); vTaskDelete(NULL); return; }
    int n = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c != '\n') {
            if (n < LINE_CAP - 1) line[n++] = (char)c; else overflow = true;
            continue;
        }
        line[n] = 0;
        if (overflow) reply("ERR line too long");
        else if (n) handle(line, buf, b64);
        n = 0;
        overflow = false;
    }
}

void usb_door_start(void)
{
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 1024, .rx_buffer_size = 1024 };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) { ESP_LOGW(TAG, "USB driver not installed: no desktop setup over USB"); return; }
    if (xTaskCreate(door_task, "usb_door", 6144, NULL, 2, NULL) != pdPASS) ESP_LOGW(TAG, "USB door task did not start");
}
