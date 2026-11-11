#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "status.h"
#include "events.h"

#if CONFIG_IDF_TARGET_ESP32S31
#include "ui_events_logic.h"
static void sys_event_text(event_level_t level, const char *text, const char *log_text);
void sys_event(event_level_t level, const char *fmt, ...)
{
    char text[EVENT_TEXT];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    sys_event_text(level, text, text);
}

void sys_event_net(event_level_t level, const char *name, const char *fmt, ...)
{
    char text[EVENT_TEXT], log_text[EVENT_TEXT + 16];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    uev_redact(text, name, "<network>", log_text, sizeof log_text);
    sys_event_text(level, text, log_text);
}

static void sys_event_text(event_level_t level, const char *text, const char *log_text)
{
#else
void sys_event(event_level_t level, const char *fmt, ...)
{
    char text[EVENT_TEXT];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
#endif
#if CONFIG_IDF_TARGET_ESP32S31
#define LOG_TEXT log_text
#else
#define LOG_TEXT text
#endif
    int64_t now_ms = esp_timer_get_time() / 1000;

    STATUS_LOCK();
    /* collapse an identical repeat of the newest entry */
    sys_event_t *newest = g_status.event_count ? &g_status.events[(g_status.event_head + EVENT_MAX - 1) % EVENT_MAX] : NULL;
    if (newest && strcmp(newest->text, text) == 0) {
        newest->uptime_ms = now_ms;
    } else {
        sys_event_t *e = &g_status.events[g_status.event_head];
        e->uptime_ms = now_ms;
        e->level = level;
        strlcpy(e->text, text, sizeof e->text);
        g_status.event_head = (g_status.event_head + 1) % EVENT_MAX;
        if (g_status.event_count < EVENT_MAX) g_status.event_count++;
    }
    if (level >= EV_WARN) {
        snprintf(g_status.sys_popup, sizeof g_status.sys_popup, "%s", text);
        g_status.sys_popup_until_ms = now_ms + 5000;
        g_status.sys_popup_error = (level == EV_ERROR);
    }
    STATUS_UNLOCK();
    if (level == EV_ERROR) ESP_LOGE("event", "%s", LOG_TEXT);
    else if (level == EV_WARN) ESP_LOGW("event", "%s", LOG_TEXT);
    else ESP_LOGI("event", "%s", LOG_TEXT);
}
