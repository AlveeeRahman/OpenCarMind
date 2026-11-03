/* Rule-based diagnostic trouble code (DTC) analysis: decodes, describes, ranks and groups
 * faults so the Faults screen can present the most important ones first.
 * Pure C, no ESP-IDF dependencies: unit-tested on the host (tests/host). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FAULT_MAX        24
#define FAULT_GROUP_MAX  6

enum { SEV_LOW = 1, SEV_MEDIUM = 2, SEV_HIGH = 3, SEV_CRITICAL = 4 };
enum { DTC_STORED = 1, DTC_PENDING = 2, DTC_PERMANENT = 4 };

typedef struct {
    char        code[6];       /* "P0301" */
    uint8_t     severity;      /* SEV_* */
    uint8_t     sources;       /* DTC_* bitmask */
    bool        is_new;        /* first seen in this session */
    int8_t      group;         /* index into report.groups or -1 */
    const char *subsystem;     /* "ignition", "fuel", ... */
    char        desc[40];      /* standard meaning */
    const char *cause;         /* likely cause, short */
} fault_t;

typedef struct {
    const char *title;         /* "Lean on both banks" */
    const char *cause;         /* shared root cause */
    uint8_t     severity;
    uint8_t     members[8];    /* indices into report.faults */
    uint8_t     n;
} fault_group_t;

typedef struct {
    fault_t       faults[FAULT_MAX];   /* ranked, most important first */
    uint8_t       n;
    fault_group_t groups[FAULT_GROUP_MAX];
    uint8_t       ngroups;
    uint8_t       max_severity;        /* 0 when no faults */
    uint8_t       new_serious;         /* new faults with severity >= SEV_HIGH since the previous analysis */
    bool          mil_on;
    char          insight[80];         /* one-line summary for the screen */
} fault_report_t;

/* Convert raw DTC bytes (pairs) into "P0300 P0171". Returns number of codes. */
int  dtc_bytes_to_string(const uint8_t *b, int n, char *out, size_t out_sz);

/* Forget which codes have been seen (call at session start). */
void fault_engine_reset(void);

/* Analyse the three space-separated code lists. Any may be NULL or empty. */
void fault_engine_analyse(const char *stored, const char *pending, const char *permanent, bool mil_on, fault_report_t *out);

const char *fault_severity_name(uint8_t sev);   /* "LOW" .. "CRITICAL" */
