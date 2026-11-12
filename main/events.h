/* System event log: a small ring of recent internal errors and notable events shown on the
 * System page and, for warnings and errors, as a short pop-up banner on any page. */
#pragma once
#include <stdint.h>

#define EVENT_MAX    10
#define EVENT_TEXT   56

typedef enum { EV_INFO = 0, EV_WARN, EV_ERROR } event_level_t;

typedef struct {
    int64_t       uptime_ms;
    event_level_t level;
    char          text[EVENT_TEXT];
} sys_event_t;

void sys_event(event_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* An event that names a Wi-Fi network (the name is the first %s of fmt). The board's screen and event list show the name;
   on the ESP-Mosaico the serial log gets the text with the name replaced by "<network>" (uev_redact), because network
   names never go to the log. The S3 build still logs the plain text (its copy of this comes with a later S3 release). */
#if CONFIG_IDF_TARGET_ESP32S31
void sys_event_net(event_level_t level, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define SYS_EVENT_NET(level, fmt, name, ...) sys_event_net(level, name, fmt, name, ##__VA_ARGS__)
#else
#define SYS_EVENT_NET(level, fmt, name, ...) sys_event(level, fmt, name, ##__VA_ARGS__)
#endif
