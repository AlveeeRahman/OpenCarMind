#include "harden.h"
#include <math.h>
#include <string.h>

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

bool harden_model_input(float *v)
{
    if (!isfinite(*v)) return false;
    if (*v > INPUT_LIMIT) *v = INPUT_LIMIT;
    else if (*v < -INPUT_LIMIT) *v = -INPUT_LIMIT;
    return true;
}
