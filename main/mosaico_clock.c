/* ESP-Mosaico Settings > Clock, the board side (mosaico_clock.h). */
#include "mosaico_clock.h"
#include <string.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "elm327.h"
#include "os.h"
#include "events.h"
#include "harden.h"
#include "mosaico_board.h"
#include "remote_config.h"
#include "sdlog.h"

static const char *TAG = "clock";

#define RTC_NOTE_MANUAL 0x4D414E31u                 /* "MAN1": the last clock source was a by-hand set */
#define RTC_NOTE_WIFI   0x31464957u                 /* "WIF1": the last clock source was a Wi-Fi sync */
static RTC_NOINIT_ATTR uint32_t s_rtc_note;         /* survives a software reset, random after power-on */
static volatile bool s_manual;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct { int y, mo, d, h, mi; char tz[48]; } s_req;
static volatile mclk_result_t s_result = MCLK_IDLE;

static clock_flags_t flags_of(const app_status_t *st)
{
    clock_flags_t f = { .wifi = st->time_synced, .manual = s_manual, .estimated = st->clock_estimated };
    return f;
}

/* app_main marks any time the RTC kept through a reset as "synced". Settings must not call that "From Wi-Fi" unless the
   last source really was Wi-Fi: with no note (an estimate, or a note lost with the RTC memory) the kept time shows as
   estimated, amber, until Wi-Fi or a by-hand set replaces it. */
void mosaico_clock_boot(void)
{
    bool note = s_rtc_note == RTC_NOTE_MANUAL, wifi_note = s_rtc_note == RTC_NOTE_WIFI;
    STATUS_LOCK();
    bool kept = g_status.time_synced;
    clock_flags_t f = flags_of(&g_status);
    clock_on_boot(&f, note);
    if (kept && !note && !wifi_note) { f.wifi = false; f.estimated = true; }
    g_status.time_synced = f.wifi;
    g_status.clock_estimated = f.estimated;
    STATUS_UNLOCK();
    if (kept && !note && !wifi_note) ESP_LOGI(TAG, "RTC kept the time but not its source: shown as estimated");
    s_manual = f.manual;
    if (!s_manual && !(kept && wifi_note)) s_rtc_note = 0;
    if (note) ESP_LOGI(TAG, "RTC note: clock was set by hand%s", s_manual ? ", kept through the reset" : ", but no time was kept");
}

void mosaico_clock_poll(void)
{
    STATUS_LOCK();
    bool wifi = g_status.time_synced;
    STATUS_UNLOCK();
    if (!wifi) return;
    if (s_manual) { s_manual = false; ESP_LOGI(TAG, "Wi-Fi time replaced the time set by hand"); }
    s_rtc_note = RTC_NOTE_WIFI;                     /* the next soft reset keeps "From Wi-Fi" */
}

clock_src_t mosaico_clock_source(const app_status_t *st)
{
    clock_flags_t f = flags_of(st);
    return clock_source(&f);
}

bool mosaico_clock_known(const app_status_t *st) { return mosaico_clock_source(st) != CLOCK_SRC_NONE; }

bool mosaico_clock_request(int y, int mo, int d, int h, int mi, const char *tz)
{
    mclk_result_t r = MCLK_BUSY;
    if (!os_allows(OS_SVC_CLOCK_SET)) r = MCLK_ERR_DRIVE;   /* the runlevels (os_core.h) */
    else if (!clock_fields_ok(y, mo, d, h, mi)) r = MCLK_ERR_TIME;
    else if (!tz || !harden_timezone_ok(tz)) r = MCLK_ERR_ZONE;
    if (r != MCLK_BUSY) { s_result = r; return false; }
    portENTER_CRITICAL(&s_mux);
    s_req.y = y; s_req.mo = mo; s_req.d = d; s_req.h = h; s_req.mi = mi;
    strlcpy(s_req.tz, tz, sizeof s_req.tz);
    portEXIT_CRITICAL(&s_mux);
    s_result = MCLK_BUSY;
    if (!mosaico_board_request(MB_CLOCK_SET, 0)) { s_result = MCLK_ERR_QUEUE; return false; }
    return true;
}

mclk_result_t mosaico_clock_result(void) { return s_result; }

const char *mosaico_clock_result_text(mclk_result_t r)
{
    switch (r) {
    case MCLK_BUSY:      return "Setting the clock...";
    case MCLK_OK:        return "Clock set.";
    case MCLK_ERR_DRIVE: return "A drive is active. End the drive first, then set the clock.";
    case MCLK_ERR_TIME:  return "That time is not valid. Pick a date from 2026 to 2099.";
    case MCLK_ERR_ZONE:  return "That time zone is not valid.";
    case MCLK_ERR_QUEUE: return "The board is busy. Try again.";
    default:             return "";
    }
}

void mosaico_clock_do_set(void)
{
    typeof(s_req) q;
    portENTER_CRITICAL(&s_mux);
    q = s_req;
    portEXIT_CRITICAL(&s_mux);
    if (!os_allows(OS_SVC_CLOCK_SET)) { s_result = MCLK_ERR_DRIVE; return; }   /* a drive may have started since the tap */
    int64_t epoch;
    if (!clock_local_to_epoch(q.tz, q.y, q.mo, q.d, q.h, q.mi, &epoch)) { s_result = MCLK_ERR_TIME; return; }
    if (!remote_config_set_timezone(q.tz)) { s_result = MCLK_ERR_ZONE; return; }
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    STATUS_LOCK();
    clock_flags_t f = flags_of(&g_status);
    clock_on_manual(&f);
    g_status.time_synced = f.wifi;
    g_status.clock_estimated = f.estimated;
    STATUS_UNLOCK();
    s_manual = true;
    s_rtc_note = RTC_NOTE_MANUAL;
    sdlog_meta_set_clock_manual(epoch * 1000, esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "clock set by hand: %04d-%02d-%02d %02d:%02d in %s = epoch %lld", q.y, q.mo, q.d, q.h, q.mi, q.tz, (long long)epoch);
    sys_event(EV_INFO, "Clock set by hand (Wi-Fi replaces it when it syncs)");
    s_result = MCLK_OK;
}
