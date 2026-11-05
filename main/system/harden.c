#include "harden.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define EPOCH_MIN 1767225600LL     /* 2026-01-01T00:00:00Z */
#define EPOCH_MAX 4102444800LL     /* 2100-01-01T00:00:00Z */
#define INPUT_LIMIT 100.0f

bool harden_epoch_plausible(int64_t epoch_s)
{
    return epoch_s >= EPOCH_MIN && epoch_s < EPOCH_MAX;
}

static bool printable(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return false;
    return true;
}

bool harden_wifi_cred_ok(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;
    size_t n = strlen(ssid), p = strlen(password);
    if (n < 1 || n > 32) return false;
    if (p != 0 && (p < 8 || p > 64)) return false;
    return printable(ssid) && printable(password);
}

bool harden_timezone_ok(const char *tz)
{
    if (!tz) return false;
    size_t n = strlen(tz);
    if (n < 3 || n > 47) return false;
    for (; *tz; tz++) {
        unsigned char c = (unsigned char)*tz;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("<>+-,.:/", c))) return false;
    }
    return true;
}

bool harden_model_input(float *v)
{
    if (!isfinite(*v)) return false;
    if (*v > INPUT_LIMIT) *v = INPUT_LIMIT;
    else if (*v < -INPUT_LIMIT) *v = -INPUT_LIMIT;
    return true;
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian), Howard Hinnant's days_from_civil. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

bool harden_http_date(const char *s, int64_t *epoch_s)
{
    static const char *mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const unsigned mdays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    /* "Sun, 06 Nov 1994 08:49:37 GMT": fixed width, digits where the format has digits */
    static const char shape[] = "aaa, dd aaa dddd dd:dd:dd GMT";
    if (!s || !epoch_s || strlen(s) != sizeof shape - 1) return false;
    for (size_t i = 0; i < sizeof shape - 1; i++) {
        char c = s[i], t = shape[i];
        if (t == 'd' ? !isdigit((unsigned char)c) : t == 'a' ? !isalpha((unsigned char)c) : c != t) return false;
    }
    unsigned d = (unsigned)atoi(s + 5), y = (unsigned)atoi(s + 12), hh = (unsigned)atoi(s + 17), mm = (unsigned)atoi(s + 20),
             ss = (unsigned)atoi(s + 23);
    char m[4] = {s[8], s[9], s[10], 0};
    int mi = -1;
    for (int i = 0; i < 12; i++) if (!strcmp(m, mon[i])) mi = i;
    if (mi < 0 || d < 1 || d > mdays[mi] || hh > 23 || mm > 59 || ss > 60) return false;
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (mi == 1 && d == 29 && !leap) return false;
    *epoch_s = days_from_civil(y, (unsigned)mi + 1, d) * 86400 + hh * 3600 + mm * 60 + (ss > 59 ? 59 : ss);
    return true;
}

bool harden_hostname_ok(const char *h)
{
    if (!h) return false;
    size_t n = strlen(h);
    if (n < 4 || n > 63 || h[0] == '.' || h[n - 1] == '.') return false;
    bool dot = false, alpha = false;
    size_t label = 0;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (c == '.') {
            if (label == 0 || h[i - 1] == '-') return false;
            dot = true; label = 0;
            continue;
        }
        if (!(isalnum((unsigned char)c) || c == '-')) return false;
        if (c == '-' && label == 0) return false;
        if (isalpha((unsigned char)c)) alpha = true;
        label++;
    }
    return dot && alpha && h[n - 1] != '-';
}
