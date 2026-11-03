#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "fault_engine.h"

/* ------------------------------------------------------------------ knowledge base */
typedef struct { const char *code; const char *sub; uint8_t sev; const char *desc; const char *cause; } kb_t;

/* Generic SAE J2012 powertrain codes. Severity: 4 stop safely soon, 3 service soon, 2 service when convenient, 1 informational. */
static const kb_t KB[] = {
    {"P0016", "ignition",     4, "Crank/cam correlation (B1 sensor A)",  "Timing chain/belt stretch, VVT, sensors"},
    {"P0017", "ignition",     4, "Crank/cam correlation (B1 sensor B)",  "Timing chain/belt stretch, VVT, sensors"},
    {"P0018", "ignition",     4, "Crank/cam correlation (B2 sensor A)",  "Timing chain/belt stretch, VVT, sensors"},
    {"P0019", "ignition",     4, "Crank/cam correlation (B2 sensor B)",  "Timing chain/belt stretch, VVT, sensors"},
    {"P0100", "air",          3, "MAF circuit malfunction",              "MAF sensor, wiring, air leak after MAF"},
    {"P0101", "air",          2, "MAF range/performance",                "Dirty MAF, intake leak, clogged filter"},
    {"P0102", "air",          3, "MAF circuit low input",                "MAF sensor or wiring"},
    {"P0103", "air",          3, "MAF circuit high input",               "MAF sensor or wiring"},
    {"P0105", "air",          3, "MAP/baro circuit malfunction",         "MAP sensor, vacuum hose, wiring"},
    {"P0106", "air",          2, "MAP/baro range/performance",           "Vacuum leak, MAP sensor"},
    {"P0110", "air",          2, "Intake air temp circuit",              "IAT sensor or wiring"},
    {"P0113", "air",          2, "Intake air temp circuit high",         "IAT sensor open circuit"},
    {"P0115", "cooling",      3, "Coolant temp circuit malfunction",     "ECT sensor or wiring"},
    {"P0117", "cooling",      3, "Coolant temp circuit low",             "ECT sensor shorted"},
    {"P0118", "cooling",      3, "Coolant temp circuit high",            "ECT sensor open circuit"},
    {"P0120", "throttle",     3, "Throttle position circuit",            "TPS sensor or wiring"},
    {"P0121", "throttle",     3, "Throttle position range/perf.",        "TPS sensor, throttle body"},
    {"P0122", "throttle",     3, "Throttle position circuit low",        "TPS sensor or wiring"},
    {"P0123", "throttle",     3, "Throttle position circuit high",       "TPS sensor or wiring"},
    {"P0125", "cooling",      2, "Coolant temp too low for closed loop", "Thermostat stuck open"},
    {"P0128", "cooling",      2, "Thermostat below regulating temp",     "Thermostat stuck open"},
    {"P0130", "emissions",    2, "O2 sensor circuit B1S1",               "Upstream O2 sensor"},
    {"P0131", "emissions",    2, "O2 sensor low voltage B1S1",           "O2 sensor, exhaust leak"},
    {"P0133", "emissions",    2, "O2 sensor slow response B1S1",         "Ageing O2 sensor"},
    {"P0134", "emissions",    2, "O2 sensor no activity B1S1",           "O2 sensor or wiring"},
    {"P0135", "emissions",    2, "O2 heater circuit B1S1",               "O2 sensor heater, fuse"},
    {"P0136", "emissions",    2, "O2 sensor circuit B1S2",               "Downstream O2 sensor"},
    {"P0141", "emissions",    2, "O2 heater circuit B1S2",               "O2 sensor heater, fuse"},
    {"P0150", "emissions",    2, "O2 sensor circuit B2S1",               "Upstream O2 sensor"},
    {"P0155", "emissions",    2, "O2 heater circuit B2S1",               "O2 sensor heater, fuse"},
    {"P0171", "fuel",         2, "System too lean (bank 1)",             "Vacuum leak, MAF, fuel pressure"},
    {"P0172", "fuel",         2, "System too rich (bank 1)",             "Leaking injector, MAF, fuel pressure"},
    {"P0174", "fuel",         2, "System too lean (bank 2)",             "Vacuum leak, MAF, fuel pressure"},
    {"P0175", "fuel",         2, "System too rich (bank 2)",             "Leaking injector, MAF, fuel pressure"},
    {"P0200", "fuel",         3, "Injector circuit malfunction",         "Injector or wiring"},
    {"P0217", "cooling",      4, "Engine over temperature",              "Coolant level, fan, thermostat"},
    {"P0218", "transmission", 4, "Transmission over temperature",        "Fluid level, cooler, heavy load"},
    {"P0230", "fuel",         3, "Fuel pump primary circuit",            "Fuel pump relay or wiring"},
    {"P0300", "ignition",     4, "Random/multiple misfire",              "Plugs, coils, fuel, compression"},
    {"P0325", "ignition",     2, "Knock sensor 1 circuit",               "Knock sensor or wiring"},
    {"P0335", "ignition",     4, "Crankshaft position sensor A",         "Crank sensor, reluctor, wiring"},
    {"P0340", "ignition",     3, "Camshaft position sensor A",           "Cam sensor, timing, wiring"},
    {"P0350", "ignition",     3, "Ignition coil circuit",                "Coil, wiring, driver"},
    {"P0400", "emissions",    2, "EGR flow malfunction",                 "EGR valve, passages"},
    {"P0401", "emissions",    2, "EGR flow insufficient",                "Clogged EGR passages"},
    {"P0402", "emissions",    2, "EGR flow excessive",                   "EGR valve stuck open"},
    {"P0410", "emissions",    2, "Secondary air injection",              "Air pump, valve, fuse"},
    {"P0420", "emissions",    2, "Catalyst efficiency low (bank 1)",     "Worn catalyst, O2 sensor"},
    {"P0430", "emissions",    2, "Catalyst efficiency low (bank 2)",     "Worn catalyst, O2 sensor"},
    {"P0440", "evap",         1, "Evap system malfunction",              "Fuel cap, hoses, valves"},
    {"P0441", "evap",         1, "Evap purge flow incorrect",            "Purge valve, hoses"},
    {"P0442", "evap",         1, "Evap small leak",                      "Fuel cap, hose"},
    {"P0443", "evap",         1, "Evap purge valve circuit",             "Purge solenoid or wiring"},
    {"P0446", "evap",         1, "Evap vent control circuit",            "Vent solenoid or wiring"},
    {"P0455", "evap",         1, "Evap large leak",                      "Fuel cap loose or missing"},
    {"P0456", "evap",         1, "Evap very small leak",                 "Fuel cap seal, hose"},
    {"P0457", "evap",         1, "Evap leak (fuel cap loose/off)",       "Tighten or replace fuel cap"},
    {"P0460", "fuel",         1, "Fuel level sensor circuit",            "Sender unit or wiring"},
    {"P0462", "fuel",         1, "Fuel level sensor low",                "Sender unit or wiring"},
    {"P0463", "fuel",         1, "Fuel level sensor high",               "Sender unit or wiring"},
    {"P0480", "cooling",      3, "Cooling fan 1 control circuit",        "Fan relay, motor, wiring"},
    {"P0500", "drivetrain",   2, "Vehicle speed sensor",                 "VSS sensor or wiring"},
    {"P0505", "throttle",     2, "Idle control system",                  "Idle valve, throttle body"},
    {"P0506", "throttle",     1, "Idle RPM lower than expected",         "Dirty throttle body, vacuum"},
    {"P0507", "throttle",     1, "Idle RPM higher than expected",        "Vacuum leak, throttle body"},
    {"P0520", "lubrication",  3, "Oil pressure sensor circuit",          "Sensor or wiring"},
    {"P0521", "lubrication",  3, "Oil pressure sensor range/perf.",      "Sensor, oil level"},
    {"P0522", "lubrication",  4, "Oil pressure sensor low voltage",      "Check oil level now"},
    {"P0523", "lubrication",  3, "Oil pressure sensor high voltage",     "Sensor or wiring"},
    {"P0524", "lubrication",  4, "Engine oil pressure too low",          "Stop safely: check oil level and pressure"},
    {"P0560", "electrical",   3, "System voltage malfunction",           "Battery, alternator, ground"},
    {"P0562", "electrical",   3, "System voltage low",                   "Alternator, battery, belt"},
    {"P0563", "electrical",   3, "System voltage high",                  "Voltage regulator"},
    {"P0600", "ecu",          2, "Serial communication link",            "Module wiring, CAN"},
    {"P0601", "ecu",          3, "ECM memory checksum error",            "ECM reflash or replace"},
    {"P0603", "ecu",          2, "ECM keep-alive memory error",          "Battery disconnect, ECM"},
    {"P0604", "ecu",          3, "ECM RAM error",                        "ECM"},
    {"P0605", "ecu",          3, "ECM ROM error",                        "ECM"},
    {"P0606", "ecu",          3, "ECM processor fault",                  "ECM"},
    {"P0700", "transmission", 3, "Transmission control system",          "Read TCM codes"},
    {"P0705", "transmission", 3, "Transmission range sensor",            "Range switch, linkage"},
    {"P0710", "transmission", 2, "Transmission fluid temp sensor",       "Sensor or wiring"},
    {"P0715", "transmission", 3, "Input/turbine speed sensor",           "Sensor or wiring"},
    {"P0720", "transmission", 3, "Output speed sensor",                  "Sensor or wiring"},
    {"P0730", "transmission", 3, "Incorrect gear ratio",                 "Fluid, clutches, solenoids"},
    {"P0740", "transmission", 2, "Torque converter clutch circuit",      "TCC solenoid, fluid"},
    {"P0750", "transmission", 3, "Shift solenoid A",                     "Solenoid or wiring"},
    {"P0A80", "hybrid",       3, "Replace hybrid battery pack",          "HV battery module"},
};
#define KB_N (sizeof KB / sizeof KB[0])

static const kb_t *kb_find(const char *code)
{
    for (size_t i = 0; i < KB_N; i++) if (strcmp(KB[i].code, code) == 0) return &KB[i];
    return NULL;
}

/* Category fallback for codes not in the table. */
static void describe_fallback(const char *code, fault_t *f)
{
    int num = (int)strtol(code + 1, NULL, 16);
    f->cause = "See service manual";
    switch (code[0]) {
    case 'C': f->subsystem = "chassis";  f->severity = SEV_HIGH;   snprintf(f->desc, sizeof f->desc, "Chassis fault (ABS/steering/susp.)"); return;
    case 'B': f->subsystem = "body";     f->severity = SEV_LOW;    snprintf(f->desc, sizeof f->desc, "Body fault (airbag/lights/HVAC)"); return;
    case 'U': f->subsystem = "network";  f->severity = SEV_MEDIUM; snprintf(f->desc, sizeof f->desc, "Network/module communication"); return;
    default: break;
    }
    if (code[1] != '0') {
        /* second character: 1 manufacturer specific, 2 generic (extended), 3 jointly defined */
        f->subsystem = "powertrain";
        f->severity = SEV_MEDIUM;
        snprintf(f->desc, sizeof f->desc, code[1] == '1' ? "Manufacturer-specific powertrain code"
                                        : code[1] == '2' ? "Powertrain code (generic, P2 range)"
                                                         : "Powertrain code (jointly defined)");
        return;
    }
    /* Cylinder and coil numbers are the last two characters read as DECIMAL (P0310 = cylinder 10);
     * P030A..P030F and P035A..P035F are reserved. */
    bool last_two_decimal = isdigit((unsigned char)code[3]) && isdigit((unsigned char)code[4]);
    int tail = last_two_decimal ? (code[3] - '0') * 10 + (code[4] - '0') : -1;
    if (strncmp(code, "P03", 3) == 0 && tail >= 1 && tail <= 12) {   /* P0301..P0312 */
        f->subsystem = "ignition"; f->severity = SEV_HIGH;
        snprintf(f->desc, sizeof f->desc, "Cylinder %d misfire detected", tail);
        f->cause = "Plug, coil, injector of that cylinder";
        return;
    }
    if (strncmp(code, "P03", 3) == 0 && tail >= 51 && tail <= 62) {  /* P0351..P0362, coils A..L */
        f->subsystem = "ignition"; f->severity = SEV_HIGH;
        snprintf(f->desc, sizeof f->desc, "Ignition coil %c circuit", 'A' + (tail - 51));
        f->cause = "Coil, connector, wiring";
        return;
    }
    int hundreds = (num >> 8) & 0xF;     /* hex digit after P0 */
    switch (hundreds) {
    case 0x0: case 0x1: case 0x2: f->subsystem = "fuel";         f->severity = SEV_MEDIUM; snprintf(f->desc, sizeof f->desc, "Fuel and air metering"); break;
    case 0x3:                     f->subsystem = "ignition";     f->severity = SEV_HIGH;   snprintf(f->desc, sizeof f->desc, "Ignition system or misfire"); break;
    case 0x4:                     f->subsystem = "emissions";    f->severity = SEV_LOW;    snprintf(f->desc, sizeof f->desc, "Auxiliary emission controls"); break;
    case 0x5:                     f->subsystem = "throttle";     f->severity = SEV_MEDIUM; snprintf(f->desc, sizeof f->desc, "Speed, idle or auxiliary inputs"); break;
    case 0x6:                     f->subsystem = "ecu";          f->severity = SEV_MEDIUM; snprintf(f->desc, sizeof f->desc, "Computer or output circuit"); break;
    case 0x7: case 0x8: case 0x9: f->subsystem = "transmission"; f->severity = SEV_HIGH;   snprintf(f->desc, sizeof f->desc, "Transmission"); break;
    case 0xA: case 0xB: case 0xC: f->subsystem = "hybrid";       f->severity = SEV_HIGH;   snprintf(f->desc, sizeof f->desc, "Hybrid propulsion"); break;
    default:                      f->subsystem = "powertrain";   f->severity = SEV_MEDIUM; snprintf(f->desc, sizeof f->desc, "Powertrain code"); break;
    }
}

static void describe(fault_t *f)
{
    const kb_t *k = kb_find(f->code);
    if (k) {
        f->subsystem = k->sub;
        f->severity = k->sev;
        snprintf(f->desc, sizeof f->desc, "%s", k->desc);
        f->cause = k->cause;
    } else describe_fallback(f->code, f);
}

/* ------------------------------------------------------------------ DTC bytes */
int dtc_bytes_to_string(const uint8_t *b, int n, char *out, size_t out_sz)
{
    static const char pfx[4] = {'P', 'C', 'B', 'U'};
    size_t used = 0;
    int count = 0;
    out[0] = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        if (b[i] == 0 && b[i + 1] == 0) continue;
        char code[8];
        snprintf(code, sizeof code, "%c%X%X%X%X", pfx[b[i] >> 6], (b[i] >> 4) & 3, b[i] & 0xF, b[i + 1] >> 4, b[i + 1] & 0xF);
        if (used + 6 >= out_sz) break;
        used += snprintf(out + used, out_sz - used, "%s%s", used ? " " : "", code);
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ session memory */
static char s_seen[FAULT_MAX * 2][6];
static int  s_seen_n;

void fault_engine_reset(void) { s_seen_n = 0; }

static bool seen_before(const char *code)
{
    for (int i = 0; i < s_seen_n; i++) if (strcmp(s_seen[i], code) == 0) return true;
    return false;
}

static void remember(const char *code)
{
    if (seen_before(code) || s_seen_n >= (int)(sizeof s_seen / sizeof s_seen[0])) return;
    snprintf(s_seen[s_seen_n++], 6, "%s", code);
}

/* ------------------------------------------------------------------ analysis */
static int find_code(const fault_report_t *r, const char *code)
{
    for (int i = 0; i < r->n; i++) if (strcmp(r->faults[i].code, code) == 0) return i;
    return -1;
}

static void add_codes(fault_report_t *r, const char *list, uint8_t source)
{
    if (!list) return;
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char code[6] = {0};
        int k = 0;
        while (*p && *p != ' ' && *p != ',' && k < 5) code[k++] = (char)toupper((unsigned char)*p++);
        while (*p && *p != ' ' && *p != ',') p++;
        if (k != 5) continue;
        int idx = find_code(r, code);
        if (idx >= 0) { r->faults[idx].sources |= source; continue; }
        if (r->n >= FAULT_MAX) continue;
        fault_t *f = &r->faults[r->n++];
        memset(f, 0, sizeof *f);
        memcpy(f->code, code, 6);
        f->sources = source;
        f->group = -1;
        describe(f);
    }
}

static bool has(const fault_report_t *r, const char *code) { return find_code(r, code) >= 0; }

static int count_misfire(const fault_report_t *r, uint8_t *members, int max)
{
    int n = 0;
    for (int i = 0; i < r->n && n < max; i++) {
        const char *c = r->faults[i].code;
        if (strncmp(c, "P03", 3) != 0 || !isdigit((unsigned char)c[3]) || !isdigit((unsigned char)c[4])) continue;
        int tail = (c[3] - '0') * 10 + (c[4] - '0');
        if (tail <= 12) members[n++] = (uint8_t)i;       /* P0300..P0312 */
    }
    return n;
}

static int count_range(const fault_report_t *r, int lo, int hi, uint8_t *members, int max)
{
    int n = 0;
    for (int i = 0; i < r->n; i++) {
        if (r->faults[i].code[0] != 'P') continue;
        int num = (int)strtol(r->faults[i].code + 1, NULL, 16);
        if (num >= lo && num <= hi && n < max) members[n++] = (uint8_t)i;
    }
    return n;
}

static void add_group(fault_report_t *r, const char *title, const char *cause, uint8_t sev, const uint8_t *members, int n)
{
    if (r->ngroups >= FAULT_GROUP_MAX || n < 2) return;
    fault_group_t *g = &r->groups[r->ngroups];
    g->title = title; g->cause = cause; g->severity = sev; g->n = 0;
    for (int i = 0; i < n && g->n < 8; i++) {
        if (r->faults[members[i]].group >= 0) continue;   /* already explained by an earlier rule */
        g->members[g->n++] = members[i];
        r->faults[members[i]].group = (int8_t)r->ngroups;
        if (sev > r->faults[members[i]].severity) r->faults[members[i]].severity = sev;
    }
    if (g->n >= 2) r->ngroups++;
    else for (int i = 0; i < g->n; i++) r->faults[g->members[i]].group = -1;
}

static void infer_groups(fault_report_t *r)
{
    uint8_t m[8];
    int n;

    /* (e) low system voltage can spawn spurious codes everywhere: fix charging first */
    if ((has(r, "P0562") || has(r, "P0560")) && r->n >= 4) {
        n = 0;
        for (int i = 0; i < r->n && n < 8; i++) m[n++] = (uint8_t)i;
        add_group(r, "Low voltage, many codes", "Battery/alternator first, then clear codes", SEV_HIGH, m, n);
    }
    /* (b) multi-cylinder misfire; (g) any misfire while the MIL is on: catalyst-damage risk */
    n = count_misfire(r, m, 8);
    if (n >= 2) add_group(r, "Misfire on several cylinders", "Fuel pressure, vacuum leak, ignition, compression", SEV_CRITICAL, m, n);
    if (n >= 1 && r->mil_on) {
        for (int i = 0; i < n; i++) if (r->faults[m[i]].severity < SEV_CRITICAL) r->faults[m[i]].severity = SEV_CRITICAL;
        if (n == 1 && r->faults[m[0]].group < 0) r->faults[m[0]].cause = "Active misfire with MIL on: reduce load, service soon";
    }
    /* (a) lean/rich on both banks */
    if (has(r, "P0171") && has(r, "P0174")) {
        m[0] = find_code(r, "P0171"); m[1] = find_code(r, "P0174");
        add_group(r, "Lean on both banks", "Vacuum leak, MAF, fuel delivery (not bank-specific)", SEV_MEDIUM, m, 2);
    }
    if (has(r, "P0172") && has(r, "P0175")) {
        m[0] = find_code(r, "P0172"); m[1] = find_code(r, "P0175");
        add_group(r, "Rich on both banks", "MAF, fuel pressure regulator, leaking injectors", SEV_MEDIUM, m, 2);
    }
    /* (f) crank and cam circuit codes together: shared supply/ground or the crank sensor, not timing */
    if (has(r, "P0335") && has(r, "P0340")) {
        m[0] = find_code(r, "P0335"); m[1] = find_code(r, "P0340");
        add_group(r, "Crank and cam circuits both faulted", "Shared wiring, power or ground; crank sensor first", SEV_CRITICAL, m, 2);
    }
    /* (c) catalyst with an upstream O2 code: sensor first */
    {
        int cat = has(r, "P0420") ? find_code(r, "P0420") : has(r, "P0430") ? find_code(r, "P0430") : -1;
        int o2 = count_range(r, 0x130, 0x167, m, 7);
        if (cat >= 0 && o2 >= 1) {
            m[o2] = (uint8_t)cat;
            add_group(r, "Catalyst code with O2 sensor fault", "Fix the O2 sensor first, then re-test the catalyst", SEV_MEDIUM, m, o2 + 1);
        }
    }
    /* (d) evap leaks: fuel cap */
    n = count_range(r, 0x440, 0x457, m, 8);
    if (n >= 2) add_group(r, "Evap leak codes", "Check the fuel cap first", SEV_LOW, m, n);
}

static int rank_cmp(const void *a, const void *b)
{
    const fault_t *x = a, *y = b;
    if (x->severity != y->severity) return y->severity - x->severity;
    if ((x->group >= 0) != (y->group >= 0)) return (y->group >= 0) - (x->group >= 0);
    if (x->is_new != y->is_new) return y->is_new - x->is_new;
    bool xs = x->sources & DTC_STORED, ys = y->sources & DTC_STORED;
    if (xs != ys) return ys - xs;
    return strcmp(x->code, y->code);
}

const char *fault_severity_name(uint8_t sev)
{
    switch (sev) {
    case SEV_CRITICAL: return "CRITICAL";
    case SEV_HIGH:     return "HIGH";
    case SEV_MEDIUM:   return "MEDIUM";
    case SEV_LOW:      return "LOW";
    default:           return "NONE";
    }
}

void fault_engine_analyse(const char *stored, const char *pending, const char *permanent, bool mil_on, fault_report_t *out)
{
    memset(out, 0, sizeof *out);
    out->mil_on = mil_on;
    add_codes(out, stored, DTC_STORED);
    add_codes(out, pending, DTC_PENDING);
    add_codes(out, permanent, DTC_PERMANENT);

    for (int i = 0; i < out->n; i++) {
        out->faults[i].is_new = !seen_before(out->faults[i].code);
    }
    infer_groups(out);

    /* sort, then remap group member indices */
    uint8_t order_code_idx[FAULT_MAX];
    (void)order_code_idx;
    qsort(out->faults, out->n, sizeof out->faults[0], rank_cmp);
    for (int g = 0; g < out->ngroups; g++) {
        fault_group_t *grp = &out->groups[g];
        int k = 0;
        for (int i = 0; i < out->n; i++) if (out->faults[i].group == g && k < 8) grp->members[k++] = (uint8_t)i;
        grp->n = k;
    }

    for (int i = 0; i < out->n; i++) {
        if (out->faults[i].severity > out->max_severity) out->max_severity = out->faults[i].severity;
        if (out->faults[i].is_new && out->faults[i].severity >= SEV_HIGH) out->new_serious++;
    }
    for (int i = 0; i < out->n; i++) remember(out->faults[i].code);

    if (out->n == 0) {
        snprintf(out->insight, sizeof out->insight, mil_on ? "MIL on but no codes read yet" : "No stored, pending or permanent codes");
    } else if (out->ngroups > 0) {
        snprintf(out->insight, sizeof out->insight, "%d fault%s, %d shared cause%s: %s", out->n, out->n > 1 ? "s" : "",
                 out->ngroups, out->ngroups > 1 ? "s" : "", out->groups[0].cause);
    } else {
        snprintf(out->insight, sizeof out->insight, "%d fault%s, worst %s: %s", out->n, out->n > 1 ? "s" : "",
                 fault_severity_name(out->max_severity), out->faults[0].desc);
    }
}
