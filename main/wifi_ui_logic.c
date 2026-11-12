#include "wifi_ui_logic.h"
#include <stdio.h>
#include <string.h>

int ws_merge(const ws_raw_t *in, int n, ws_row_t *out, int cap, int max, ws_saved_fn saved, void *ctx, int *n_seen)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        const ws_raw_t *r = &in[i];
        size_t len = strnlen(r->ssid, sizeof r->ssid);
        if (len == 0 || len >= sizeof r->ssid) continue;              /* hidden, or not terminated */
        int j = 0;
        while (j < k && strcmp(out[j].ssid, r->ssid) != 0) j++;
        if (j < k) {                                                    /* the same name again: keep the stronger */
            if (r->rssi > out[j].rssi) { out[j].rssi = r->rssi; out[j].secure = r->secure; }
            continue;
        }
        if (k >= cap) {                                                 /* scratch full: replace the weakest if weaker */
            int m = 0;
            for (int q = 1; q < k; q++) if (out[q].rssi < out[m].rssi) m = q;
            if (k == 0 || r->rssi <= out[m].rssi) continue;
            memmove(&out[m], &out[m + 1], (size_t)(k - m - 1) * sizeof out[0]);   /* keeps the sighting order */
            k--;
        }
        ws_row_t *o = &out[k++];
        memset(o, 0, sizeof *o);
        memcpy(o->ssid, r->ssid, len + 1);
        o->rssi = r->rssi;
        o->secure = r->secure;
    }
    /* insertion sort, strongest first; equal signal keeps the first sighting first (stable) */
    for (int i = 1; i < k; i++) {
        ws_row_t t = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < t.rssi) { out[j + 1] = out[j]; j--; }
        out[j + 1] = t;
    }
    if (n_seen) *n_seen = k;
    if (k > max) k = max;
    for (int i = 0; i < k; i++) {
        out[i].saved = saved ? saved(out[i].ssid, ctx) : false;
        out[i].joinable = wv_check(out[i].ssid, "", false, false) == WV_OK;
    }
    return k;
}

int ws_bars(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -66) return 3;
    if (rssi >= -77) return 2;
    return 1;
}

/* length of the UTF-8 sequence starting at s (RFC 3629: no overlongs, no surrogates, nothing above U+10FFFF), 0 if broken */
static int utf8_seq(const unsigned char *s)
{
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    int n;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
    else return 0;
    if (s[1] < lo || s[1] > hi) return 0;
    for (int i = 2; i < n; i++) if (s[i] < 0x80 || s[i] > 0xBF) return 0;
    return n;
}

static bool ctrl(unsigned char c) { return c < 0x20 || c == 0x7F; }

bool ws_utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int k = utf8_seq(p);
        if (!k) return false;
        p += k;
    }
    return true;
}

void ws_display_name(const char *ssid, char *out, size_t n)
{
    if (!n) return;
    const unsigned char *p = (const unsigned char *)ssid;
    size_t o = 0;
    while (*p) {
        int k = utf8_seq(p);
        if (k == 1 && !ctrl(*p)) {
            if (o + 1 >= n) break;
            out[o++] = (char)*p++;
        } else if (k > 1) {
            if (o + (size_t)k >= n) break;                    /* never cut a character in half */
            memcpy(out + o, p, (size_t)k);
            o += (size_t)k;
            p += k;
        } else {
            if (o + 1 >= n) break;
            out[o++] = '?';
            p++;
        }
    }
    out[o] = 0;
}

static bool has_ctrl(const char *s)
{
    for (; *s; s++) if (ctrl((unsigned char)*s)) return true;
    return false;
}

wv_result_t wv_check(const char *ssid, const char *pw, bool secure, bool have_saved)
{
    size_t n = strlen(ssid), p = strlen(pw);
    if (n < 1 || n > 32) return WV_SSID_LEN;
    if (has_ctrl(ssid)) return WV_SSID_CTRL;
    if (p == 0) return secure && !have_saved ? WV_PW_NEEDED : WV_OK;
    if (p < 8) return WV_PW_SHORT;
    if (p > 64) return WV_PW_LONG;
    if (has_ctrl(pw)) return WV_PW_CTRL;
    return WV_OK;
}

const char *wv_text(wv_result_t r)
{
    switch (r) {
    case WV_OK:        return "";
    case WV_SSID_LEN:  return "The network name must be 1 to 32 bytes.";
    case WV_SSID_CTRL: return "This name has control characters: it can't be saved.";
    case WV_PW_NEEDED: return "Type the network's password.";
    case WV_PW_SHORT:  return "Too short: passwords are 8 to 64 characters.";
    case WV_PW_LONG:   return "Too long: passwords are 8 to 64 characters.";
    case WV_PW_CTRL:   return "The password has a control character.";
    }
    return "";
}

wj_result_t wj_classify(bool got_ip, int reason)
{
    if (got_ip) return WJ_CONNECTED;
    switch (reason) {
    case 15:                       /* 4WAY_HANDSHAKE_TIMEOUT: the guide's "wrong password" code */
    case 204:                      /* HANDSHAKE_TIMEOUT: Espressif's name for the same failure */
    case 202:                      /* AUTH_FAIL: what a WPA3-SAE access point answers to a wrong password */
    case 14:                       /* MIC_FAILURE: the handshake's integrity check failed (wrong key) */
        return WJ_WRONG_PASSWORD;
    case 210:                      /* NO_AP_FOUND_W_COMPATIBLE_SECURITY */
    case 211:                      /* NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: e.g. WEP or WPA (TKIP) only */
        return WJ_SECURITY;
    default:                       /* 201 NO_AP_FOUND, 200 BEACON_TIMEOUT, 203 ASSOC_FAIL, 212 RSSI, 0 = no address in time */
        return WJ_NOT_REACHED;
    }
}

const char *wj_text(wj_result_t r)
{
    switch (r) {
    case WJ_NONE:           return "";
    case WJ_WAIT:           return "Joining...";
    case WJ_CONNECTED:      return "Connected. Setting the clock now.";
    case WJ_WRONG_PASSWORD: return "Wrong password. Check it and try again.";
    case WJ_NOT_REACHED:    return "Can't reach it. It stays saved: the board tries it again for the clock.";
    case WJ_SECURITY:       return "Can't join: the board does not support this network's security type.";
    case WJ_FULL:           return "The board holds 8 networks, the most it can. Forget one below first.";
    case WJ_BUSY:           return "Wi-Fi is busy. Try again in a moment.";
    case WJ_DRIVING:        return "Wi-Fi stays off while the car is being logged.";
    case WJ_NO_RADIO:       return "Wi-Fi could not start. Restart the board and try again.";
    case WJ_INVALID:        return "That name or password can't be saved.";
    case WJ_SAVE_FAILED:    return "The network could not be saved. Try again.";
    }
    return "";
}

bool wj_forget_after(wj_result_t r) { return r == WJ_WRONG_PASSWORD || r == WJ_SECURITY; }

void ws_state_text(int state, int n, int n_seen, char *out, size_t len)
{
    switch (state) {
    case WS_ST_SEARCHING: snprintf(out, len, "Searching..."); break;
    case WS_ST_DONE:
        if (n == 0) snprintf(out, len, "No networks found. Move closer to the router and search again.");
        else if (n_seen > n) snprintf(out, len, "%d networks, the %d strongest shown. Tap one to join.", n_seen, n);
        else snprintf(out, len, "%d network%s found. Tap one to join.", n, n == 1 ? "" : "s");
        break;
    case WS_ST_FAILED:    snprintf(out, len, "Search failed: Wi-Fi is busy or could not start. Try again in a moment."); break;
    case WS_ST_DRIVING:   snprintf(out, len, "Wi-Fi stays off while the car is being logged."); break;
    default:              snprintf(out, len, "Tap Search to list the networks in range."); break;
    }
}

void ws_wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

bool wradio_get(int *users, bool started)
{
    if (started) (*users)++;
    return started;
}

bool wradio_put(int *users)
{
    if (*users <= 0) return false;
    return --*users == 0;
}
