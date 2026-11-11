/* wifi_ui_logic.c: the ESP-Mosaico Wi-Fi app's search list (merge, dedupe, sort, hidden names, the saved mark), the
   password rules (the same as the USB door's WIFI-ADD, harden_wifi_cred_ok, checked against it on every case and on
   random input), the UTF-8 name shown on screen, the join result from the station's disconnect reason, and the texts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_ui_logic.h"
#include "harden.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static ws_raw_t R(const char *s, int rssi, bool sec)
{
    ws_raw_t r;
    memset(&r, 0, sizeof r);
    strncpy(r.ssid, s, sizeof r.ssid - 1);
    r.rssi = (int8_t)rssi;
    r.secure = sec;
    return r;
}

static bool saved_home(const char *ssid, void *ctx) { (void)ctx; return !strcmp(ssid, "Home") || !strcmp(ssid, "Shop"); }

/* the screen accepts exactly what WIFI-ADD accepts, for the pair it sends */
static void agree(const char *ssid, const char *pw)
{
    bool door = harden_wifi_cred_ok(ssid, pw);
    bool screen = wv_check(ssid, pw, false, false) == WV_OK;
    if (door != screen) printf("  disagree on ssid len %zu pw len %zu\n", strlen(ssid), strlen(pw));
    CHECK(door == screen);
}

static void rep(char *out, const char *unit, int times)
{
    out[0] = 0;
    for (int i = 0; i < times; i++) strcat(out, unit);
}

int main(void)
{
    ws_row_t out[48];
    int seen = -1;

    /* ---- merge: duplicates, hidden, order, saved ---- */
    {
        ws_raw_t in[] = { R("Cafe", -70, false), R("Home", -50, true), R("", -40, true), R("Cafe", -60, false),
                          R("Home", -80, true), R("Shop", -60, true), R("Guest", -90, false) };
        int n = ws_merge(in, 7, out, 48, WS_ROWS_MAX, saved_home, NULL, &seen);
        CHECK(n == 4 && seen == 4);
        CHECK(!strcmp(out[0].ssid, "Home") && out[0].rssi == -50 && out[0].secure && out[0].saved);
        /* Cafe -60 (its stronger record) and Shop -60 tie: Cafe was seen first */
        CHECK(!strcmp(out[1].ssid, "Cafe") && out[1].rssi == -60 && !out[1].secure && !out[1].saved);
        CHECK(!strcmp(out[2].ssid, "Shop") && out[2].saved);
        CHECK(!strcmp(out[3].ssid, "Guest") && out[3].rssi == -90);
        for (int i = 0; i < n; i++) CHECK(out[i].joinable);
        for (int i = 1; i < n; i++) CHECK(out[i - 1].rssi >= out[i].rssi);
    }
    /* a name seen open and secured keeps the strongest record's security */
    {
        ws_raw_t in[] = { R("Mixed", -70, false), R("Mixed", -55, true) };
        CHECK(ws_merge(in, 2, out, 48, WS_ROWS_MAX, NULL, NULL, NULL) == 1 && out[0].secure && out[0].rssi == -55);
    }
    /* only hidden names: nothing; no input: nothing */
    {
        ws_raw_t in[] = { R("", -30, true), R("", -50, false) };
        CHECK(ws_merge(in, 2, out, 48, WS_ROWS_MAX, NULL, NULL, &seen) == 0 && seen == 0);
        CHECK(ws_merge(in, 0, out, 48, WS_ROWS_MAX, NULL, NULL, &seen) == 0 && seen == 0);
    }
    /* a record whose name fills all 33 bytes without a NUL is dropped, a full 32-byte name is kept */
    {
        ws_raw_t in[2];
        memset(&in, 0, sizeof in);
        memset(in[0].ssid, 'A', 33);
        in[0].rssi = -40;
        memset(in[1].ssid, 'B', 32);
        in[1].rssi = -41;
        CHECK(ws_merge(in, 2, out, 48, WS_ROWS_MAX, NULL, NULL, NULL) == 1 && strlen(out[0].ssid) == 32 && out[0].ssid[0] == 'B');
    }
    /* more names than shown: the 20 strongest, strongest first, n_seen counts all */
    {
        ws_raw_t in[40];
        char nm[16];
        for (int i = 0; i < 40; i++) { snprintf(nm, sizeof nm, "net%02d", i); in[i] = R(nm, -95 + i, i & 1); }
        int n = ws_merge(in, 40, out, 48, WS_ROWS_MAX, NULL, NULL, &seen);
        CHECK(n == WS_ROWS_MAX && seen == 40);
        CHECK(!strcmp(out[0].ssid, "net39") && out[0].rssi == -56);
        CHECK(!strcmp(out[WS_ROWS_MAX - 1].ssid, "net20"));
    }
    /* scratch smaller than the names, input weakest first: the strongest still win */
    {
        ws_raw_t in[30];
        char nm[16];
        for (int i = 0; i < 30; i++) { snprintf(nm, sizeof nm, "w%02d", i); in[i] = R(nm, -100 + i, false); }
        int n = ws_merge(in, 30, out, 8, 5, NULL, NULL, &seen);
        CHECK(n == 5 && seen == 8);
        CHECK(!strcmp(out[0].ssid, "w29") && !strcmp(out[4].ssid, "w25"));
    }
    /* random scans: no duplicate, no hidden, sorted, every kept name present in the input with its best signal */
    srand(7);
    for (int t = 0; t < 300; t++) {
        ws_raw_t in[40];
        int n = rand() % 41;
        for (int i = 0; i < n; i++) {
            char nm[8];
            int id = rand() % 12;
            if (id == 0) nm[0] = 0; else snprintf(nm, sizeof nm, "n%d", id);
            in[i] = R(nm, -30 - rand() % 70, rand() & 1);
        }
        int k = ws_merge(in, n, out, 48, WS_ROWS_MAX, NULL, NULL, &seen);
        bool ok = k == seen && k <= 11;
        for (int i = 0; i < k && ok; i++) {
            if (!out[i].ssid[0]) ok = false;
            if (i && out[i - 1].rssi < out[i].rssi) ok = false;
            for (int j = 0; j < i; j++) if (!strcmp(out[i].ssid, out[j].ssid)) ok = false;
            int best = -128;
            for (int j = 0; j < n; j++) if (!strcmp(in[j].ssid, out[i].ssid) && in[j].rssi > best) best = in[j].rssi;
            if (best != out[i].rssi) ok = false;
        }
        int distinct = 0;
        for (int i = 0; i < n; i++) {
            if (!in[i].ssid[0]) continue;
            int j = 0;
            while (j < i && strcmp(in[j].ssid, in[i].ssid)) j++;
            if (j == i) distinct++;
        }
        if (distinct != k) ok = false;
        CHECK(ok);
    }

    /* ---- signal bars ---- */
    CHECK(ws_bars(-30) == 4 && ws_bars(-55) == 4 && ws_bars(-56) == 3 && ws_bars(-66) == 3);
    CHECK(ws_bars(-67) == 2 && ws_bars(-77) == 2 && ws_bars(-78) == 1 && ws_bars(-100) == 1);

    /* ---- names on screen: UTF-8 kept, broken bytes and control bytes as '?' ---- */
    {
        char d[64];
        ws_display_name("Caf\xC3\xA9 W\xE2\x82\xAC", d, sizeof d);              /* "Café W€" */
        CHECK(!strcmp(d, "Caf\xC3\xA9 W\xE2\x82\xAC"));
        CHECK(ws_utf8_valid("Caf\xC3\xA9") && ws_utf8_valid("\xF0\x9F\x9A\x97 car"));
        ws_display_name("A\xC3" "B", d, sizeof d);                               /* a cut sequence */
        CHECK(!strcmp(d, "A?B"));
        ws_display_name("x\xC0\xAFy", d, sizeof d);                              /* overlong '/' */
        CHECK(!strcmp(d, "x??y"));
        CHECK(!ws_utf8_valid("x\xC0\xAFy") && !ws_utf8_valid("\xED\xA0\x80") && !ws_utf8_valid("\xF4\x90\x80\x80"));
        ws_display_name("tab\there\x7f", d, sizeof d);
        CHECK(!strcmp(d, "tab?here?"));
        ws_display_name("ab\xE2\x82\xAC", d, 5);                                 /* "ab" + 3-byte char does not fit in 5 */
        CHECK(!strcmp(d, "ab"));
        ws_display_name("abcdef", d, 4);
        CHECK(!strcmp(d, "abc"));
        ws_display_name("", d, sizeof d);
        CHECK(d[0] == 0);
    }

    /* ---- the password rules: WIFI-ADD's, counted in bytes ---- */
    {
        char s[128], p[128];
        CHECK(wv_check("Home", "secret12", true, false) == WV_OK);
        CHECK(wv_check("Home", "", true, false) == WV_PW_NEEDED);
        CHECK(wv_check("Home", "", true, true) == WV_OK);                        /* saved: an empty field keeps it */
        CHECK(wv_check("Cafe", "", false, false) == WV_OK);                       /* open network */
        CHECK(wv_check("Home", "short7!", true, false) == WV_PW_SHORT);
        rep(p, "a", 8);  CHECK(wv_check("Home", p, true, false) == WV_OK);
        rep(p, "a", 63); CHECK(wv_check("Home", p, true, false) == WV_OK);
        rep(p, "f", 64); CHECK(wv_check("Home", p, true, false) == WV_OK);        /* 64 hex digits: a raw key */
        rep(p, "a", 65); CHECK(wv_check("Home", p, true, false) == WV_PW_LONG);
        CHECK(wv_check("Home", "bad\npass1", true, false) == WV_PW_CTRL);
        CHECK(wv_check("Home", "bad\x7fpass1", true, false) == WV_PW_CTRL);
        CHECK(wv_check("", "secret12", true, false) == WV_SSID_LEN);
        rep(s, "N", 32); CHECK(wv_check(s, "secret12", true, false) == WV_OK);
        rep(s, "N", 33); CHECK(wv_check(s, "secret12", true, false) == WV_SSID_LEN);
        CHECK(wv_check("Ho\x01me", "secret12", true, false) == WV_SSID_CTRL);
        /* UTF-8: bytes count, not characters (the SSID field is 32 bytes) */
        rep(s, "\xC3\xA9", 16); CHECK(strlen(s) == 32 && wv_check(s, "", false, false) == WV_OK);
        rep(s, "\xC3\xA9", 17); CHECK(wv_check(s, "", false, false) == WV_SSID_LEN);
        rep(s, "\xE2\x82\xAC", 11); CHECK(strlen(s) == 33 && wv_check(s, "", false, false) == WV_SSID_LEN);
        rep(p, "\xC3\xA9", 4); CHECK(wv_check("Home", p, true, false) == WV_OK);  /* 4 characters, 8 bytes */
        rep(p, "\xC3\xA9", 3); CHECK(wv_check("Home", p, true, false) == WV_PW_SHORT);
        rep(p, "\xE2\x82\xAC", 22); CHECK(strlen(p) == 66 && wv_check("Home", p, true, false) == WV_PW_LONG);
        /* a row is joinable when its name passes the rules */
        ws_raw_t in[] = { R("Ok", -50, true), R("Bad\x02", -40, true) };
        int n = ws_merge(in, 2, out, 48, WS_ROWS_MAX, NULL, NULL, NULL);
        CHECK(n == 2 && !out[0].joinable && out[1].joinable);
        /* every check agrees with the door */
        const char *ss[] = { "Home", "", "Ho\x01me", "\xC3\xA9\xC3\xA9", "x" };
        const char *pp[] = { "", "a", "1234567", "12345678", "bad\tpass1", "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9" };
        for (size_t i = 0; i < sizeof ss / sizeof *ss; i++) for (size_t j = 0; j < sizeof pp / sizeof *pp; j++) agree(ss[i], pp[j]);
        for (int len = 0; len <= 70; len++) { rep(p, "k", len); agree("Home", p); rep(s, "S", len < 40 ? len : 40); agree(s, "secret12"); }
        srand(11);
        for (int t = 0; t < 3000; t++) {
            int ls = rand() % 36, lp = rand() % 70;
            for (int i = 0; i < ls; i++) s[i] = (char)(1 + rand() % 255);
            s[ls] = 0;
            for (int i = 0; i < lp; i++) p[i] = (char)(rand() % 8 ? 0x20 + rand() % 95 : 1 + rand() % 255);
            p[lp] = 0;
            agree(s, p);
        }
    }

    /* ---- join results: the reason codes of esp_wifi_types_generic.h ---- */
    CHECK(wj_classify(true, 0) == WJ_CONNECTED);
    CHECK(wj_classify(true, 15) == WJ_CONNECTED);                                 /* an address wins over an old reason */
    CHECK(wj_classify(false, 15) == WJ_WRONG_PASSWORD);
    CHECK(wj_classify(false, 204) == WJ_WRONG_PASSWORD);
    CHECK(wj_classify(false, 202) == WJ_WRONG_PASSWORD);
    CHECK(wj_classify(false, 14) == WJ_WRONG_PASSWORD);
    CHECK(wj_classify(false, 201) == WJ_NOT_REACHED);
    CHECK(wj_classify(false, 200) == WJ_NOT_REACHED);
    CHECK(wj_classify(false, 203) == WJ_NOT_REACHED);
    CHECK(wj_classify(false, 212) == WJ_NOT_REACHED);
    CHECK(wj_classify(false, 0) == WJ_NOT_REACHED);                                /* no address within the wait */
    CHECK(wj_classify(false, 210) == WJ_SECURITY && wj_classify(false, 211) == WJ_SECURITY);
    CHECK(wj_forget_after(WJ_WRONG_PASSWORD) && wj_forget_after(WJ_SECURITY));
    CHECK(!wj_forget_after(WJ_NOT_REACHED) && !wj_forget_after(WJ_CONNECTED) && !wj_forget_after(WJ_FULL));

    /* ---- texts: the three outcomes the owner asked for, every state has a text that fits a 424 px row ---- */
    CHECK(!strncmp(wj_text(WJ_CONNECTED), "Connected", 9));
    CHECK(!strncmp(wj_text(WJ_WRONG_PASSWORD), "Wrong password", 14));
    CHECK(!strncmp(wj_text(WJ_NOT_REACHED), "Can't reach it", 14));
    CHECK(strstr(wj_text(WJ_FULL), "8 networks") && strstr(wj_text(WJ_FULL), "Forget"));
    for (int r = WJ_WAIT; r <= WJ_SAVE_FAILED; r++) CHECK(wj_text((wj_result_t)r)[0] && strlen(wj_text((wj_result_t)r)) < 80);
    CHECK(wj_text(WJ_NONE)[0] == 0);
    for (int r = WV_SSID_LEN; r <= WV_PW_CTRL; r++) CHECK(wv_text((wv_result_t)r)[0] && strlen(wv_text((wv_result_t)r)) < 64);
    CHECK(wv_text(WV_OK)[0] == 0);
    {
        char b[96];
        ws_state_text(WS_ST_NONE, 0, 0, b, sizeof b);      CHECK(strstr(b, "Search") != NULL);
        ws_state_text(WS_ST_SEARCHING, 0, 0, b, sizeof b); CHECK(!strcmp(b, "Searching..."));
        ws_state_text(WS_ST_DONE, 0, 0, b, sizeof b);      CHECK(!strncmp(b, "No networks found", 17));
        ws_state_text(WS_ST_DONE, 1, 1, b, sizeof b);      CHECK(!strcmp(b, "1 network found. Tap one to join."));
        ws_state_text(WS_ST_DONE, 5, 5, b, sizeof b);      CHECK(!strcmp(b, "5 networks found. Tap one to join."));
        ws_state_text(WS_ST_DONE, 20, 31, b, sizeof b);    CHECK(!strcmp(b, "31 networks, the 20 strongest shown. Tap one to join."));
        ws_state_text(WS_ST_FAILED, 0, 0, b, sizeof b);    CHECK(!strncmp(b, "Search failed", 13));
        ws_state_text(WS_ST_DRIVING, 0, 0, b, sizeof b);   CHECK(strstr(b, "logged") != NULL);
        ws_state_text(WS_ST_DONE, 20, 31, b, 10);          CHECK(strlen(b) == 9);      /* truncates, stays terminated */
    }
    /* ---- the wipe ---- */
    {
        char pw[65] = "hunter2hunter2";
        ws_wipe(pw, sizeof pw);
        int nz = 0;
        for (size_t i = 0; i < sizeof pw; i++) nz += pw[i] != 0;
        CHECK(nz == 0);
    }

    printf("wifi_ui: %d checks, %d failures\n", runs, fails);
    return fails ? 1 : 0;
}
