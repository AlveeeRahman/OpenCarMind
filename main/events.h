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
