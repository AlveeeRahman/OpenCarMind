/* libFuzzer harness for the parsers that read data from outside the board: HTTP dates and host names (clock sync),
 * Wi-Fi entries and time zones (config.json, wifi.csv), custom PID formulas (custom_pids.csv), Bluetooth names and
 * UUIDs (adapter scan), and trouble-code bytes (the car). Every one is pure C and already unit-tested in tests/host;
 * fuzzing looks for the inputs the unit tests did not think of.
 * Build and run:  make -C tests/fuzz   (needs clang with -fsanitize=fuzzer). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adapter_match.h"
#include "custom_pids.h"
#include "events.h"
#include "fault_engine.h"
#include "harden.h"

/* Host replacement for the firmware event log, so custom_pids.c links. */
void sys_event(event_level_t level, const char *fmt, ...)
{
    (void)level; (void)fmt;
}

/* Results go into a volatile sink so the compiler keeps every call. */
static volatile int sink;

/* The first byte picks the parser, the rest is its input; a NUL-terminated copy serves the string parsers. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;
    uint8_t which = data[0] % 8;
    const uint8_t *in = data + 1;
    size_t n = size - 1;
    char *s = malloc(n + 1);
    if (!s) return 0;
    memcpy(s, in, n);
    s[n] = 0;

    switch (which) {
    case 0: { int64_t e; sink ^= harden_http_date(s, &e); break; }
    case 1: sink ^= harden_hostname_ok(s); break;
    case 2: {
        char *comma = memchr(s, ',', n);
        if (comma) { *comma = 0; sink ^= harden_wifi_cred_ok(s, comma + 1); } else sink ^= harden_wifi_cred_ok(s, "");
        break;
    }
    case 3: sink ^= harden_timezone_ok(s); break;
    case 4: {
        uint8_t bytes[8] = {0x1A, 0xF8, 0x80, 0x05, 0x00, 0xFF, 0x7F, 0x01};
        float v;
        sink ^= custom_pid_eval(s, bytes, (int)(n % 9), &v);
        break;
    }
    case 5: {
        adapter_uuid_t u;
        sink ^= adapter_uuid_parse(s, &u);
        sink ^= adapter_name_matches((const char *)in, n, "OBD,LELink,Vgate,vLinker");
        sink ^= adapter_name_looks_obd((const char *)in, n);
        break;
    }
    case 6: {
        char out[128];
        sink ^= dtc_bytes_to_string(in, (int)(n > 64 ? 64 : n), out, sizeof out);
        break;
    }
    default: {
        /* Three code strings separated by '|', as the fault engine receives them from the three OBD modes. */
        char *a = s, *b = strchr(a, '|'), *c = NULL;
        if (b) { *b++ = 0; c = strchr(b, '|'); if (c) *c++ = 0; }
        fault_report_t rep;
        fault_engine_reset();
        fault_engine_analyse(a, b ? b : "", c ? c : "", n & 1, &rep);
        break;
    }
    }
    free(s);
    return 0;
}
