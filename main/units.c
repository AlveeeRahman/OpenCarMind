#include "units.h"
#include "nvs.h"
#include "esp_log.h"
#include "status.h"
#include "events.h"

static const char *TAG = "units";
static int8_t s_locked = -1;     /* detected once and stored, -1 = not yet */
static int8_t s_override = -1;   /* config.json, -1 = auto */

static void publish(void)
{
    units_t u = units_current();
    STATUS_LOCK(); g_status.units = (uint8_t)u; STATUS_UNLOCK();
}

void units_init(void)
{
    nvs_handle_t h;
    if (nvs_open("units", NVS_READONLY, &h) == ESP_OK) {
        int8_t v;
        if (nvs_get_i8(h, "lock", &v) == ESP_OK && v >= UNITS_METRIC && v <= UNITS_UK) s_locked = v;
        if (nvs_get_i8(h, "ovr", &v) == ESP_OK && v >= -1 && v <= UNITS_UK) s_override = v;
        nvs_close(h);
    }
    publish();
    ESP_LOGI(TAG, "units %s (%s)", units_name(units_current()), s_override >= 0 ? "set in config" : s_locked >= 0 ? "detected" : "default");
}

units_t units_current(void)
{
    if (s_override >= 0) return (units_t)s_override;
    if (s_locked >= 0) return (units_t)s_locked;
    return UNITS_METRIC;
}

static void store(const char *key, int8_t v)
{
    nvs_handle_t h;
    if (nvs_open("units", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

void units_set_override(int u)
{
    if (u < -1 || u > UNITS_UK || u == s_override) return;
    s_override = (int8_t)u;
    store("ovr", s_override);
    publish();
}

void units_detect(const char *country, const char *tz)
{
    if (s_locked >= 0) return;                       /* decided once: keep it */
    int u = units_from_country(country);
    const char *why = "Wi-Fi country";
    if (u < 0) { u = units_from_timezone(tz); why = "time zone"; }
    if (u < 0) return;                               /* nothing to go on yet: stay metric, try at the next connect */
    s_locked = (int8_t)u;
    store("lock", s_locked);
    publish();
    sys_event(EV_INFO, "Units: %s (from %s %s)", u == UNITS_METRIC ? "km, km/L" : "miles, mpg",
              why, u >= 0 && country && country[0] ? country : "");
}
