/* ESP-Mosaico Wi-Fi app: the parts of "search and join" that need no radio and no LVGL, so the host tests can run them
 * (tests/host/test_wifi_ui.c). The search list: merge the scan records (one row per name, the strongest kept, hidden
 * networks left out), sort strongest first, mark the saved ones. The password check: the same rules as the USB door's
 * WIFI-ADD (harden_wifi_cred_ok: SSID 1-32 bytes, password empty or 8-64 bytes, no control bytes), plus "a secured
 * network needs a password" for the screen. The join result: the station's disconnect reason mapped to what the screen
 * says. Network names and passwords never go to a log from here. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_ROWS_MAX 20                     /* rows the search list shows (the strongest ones) */

typedef struct { char ssid[33]; int8_t rssi; bool secure; } ws_raw_t;   /* one scan record, ssid NUL-terminated */
typedef struct {
    char   ssid[33];                       /* the raw bytes, for the join */
    int8_t rssi;
    bool   secure, saved;
    bool   joinable;                       /* the name passes the WIFI-ADD rules (no control bytes) */
} ws_row_t;

/* saved(ssid, ctx): is this exact name in the saved list */
typedef bool (*ws_saved_fn)(const char *ssid, void *ctx);
/* in: n records in any order (duplicates, empty names allowed). out: scratch for cap distinct names (when more come,
   the weakest gives way); on return its first rows, at most max, are the strongest, strongest first, equal signal in the
   order of the first sighting. A name seen open and secured keeps its strongest record. *n_seen (may be NULL) gets the
   distinct names kept before the max cut. Returns the rows. */
int ws_merge(const ws_raw_t *in, int n, ws_row_t *out, int cap, int max, ws_saved_fn saved, void *ctx, int *n_seen);
/* signal bars 1-4 for a visible network: >= -55 dBm 4, >= -66 3, >= -77 2, else 1 */
int ws_bars(int rssi);
/* a name for the screen: valid UTF-8 copied, every byte of a broken sequence and every control byte shown as '?' */
void ws_display_name(const char *ssid, char *out, size_t n);
bool ws_utf8_valid(const char *s);

typedef enum {
    WV_OK = 0,
    WV_SSID_LEN,           /* 0 or more than 32 bytes */
    WV_SSID_CTRL,          /* a control byte in the name */
    WV_PW_NEEDED,          /* secured network, nothing typed (and nothing saved to fall back on) */
    WV_PW_SHORT,           /* 1-7 bytes */
    WV_PW_LONG,            /* more than 64 bytes */
    WV_PW_CTRL,            /* a control byte in the password */
} wv_result_t;
/* secure: the network asks for a password; have_saved: an empty field means "use the saved password" */
wv_result_t wv_check(const char *ssid, const char *pw, bool secure, bool have_saved);
const char *wv_text(wv_result_t r);

typedef enum {
    WJ_NONE = 0,           /* no join asked yet */
    WJ_WAIT,               /* handed to the Wi-Fi task, waiting for its turn or connecting */
    WJ_CONNECTED,
    WJ_WRONG_PASSWORD,
    WJ_NOT_REACHED,        /* not found, timed out, no address */
    WJ_SECURITY,           /* found, but its security type is one the board does not join */
    WJ_FULL,               /* the store holds WIFI_CREDS_MAX networks */
    WJ_BUSY,               /* a join is already waiting, or the Wi-Fi task is starting */
    WJ_DRIVING,            /* the car is being logged: Wi-Fi stays off */
    WJ_NO_RADIO,           /* Wi-Fi could not start (internal RAM) or is compiled out */
    WJ_INVALID,            /* the name or password failed the WIFI-ADD rules */
    WJ_SAVE_FAILED,        /* the NVS write failed */
} wj_result_t;
/* got_ip: the station got an address within the wait. reason: the last WIFI_EVENT_STA_DISCONNECTED reason, 0 for none
   (esp_wifi_types_generic.h; the ESP-IDF station guide lists 15, 204 and 202 for a wrong password) */
wj_result_t wj_classify(bool got_ip, int reason);
const char *wj_text(wj_result_t r);
/* after a failed join: drop the network again (a wrong password must not stay saved) */
bool wj_forget_after(wj_result_t r);

/* the search line under the button. state: 0 never searched, 1 searching, 2 done (n rows, n_seen names before the cap),
   3 failed, 4 refused while driving */
enum { WS_ST_NONE = 0, WS_ST_SEARCHING, WS_ST_DONE, WS_ST_FAILED, WS_ST_DRIVING };
void ws_state_text(int state, int n, int n_seen, char *out, size_t len);

/* overwrites a buffer that held a password (the compiler may not drop it) */
void ws_wipe(void *p, size_t n);

/* On-demand radio count (wifi_sync.c, S31): a user takes the driver with wradio_get(&users, started), where started says
   whether the driver is (or just came) up; it counts only then. wradio_put() gives it back and returns true when the
   last user left, so the caller stops and de-initialises the driver. A put with no user is ignored. */
bool wradio_get(int *users, bool started);
bool wradio_put(int *users);
