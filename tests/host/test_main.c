/* Host unit tests for the pure-C modules: formula evaluator, custom PID loader, DTC decoding, fault engine.
 * Build and run:  make -C tests/host   (needs clang or gcc). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "custom_pids.h"
#include "fault_engine.h"
#include "pattern.h"
#include "events.h"
#include <stdarg.h>

/* Host replacement for the firmware event log (main/events.c needs FreeRTOS), so custom_pids.c links. */
void sys_event(event_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "event[%d]: ", (int)level);
    vfprintf(stderr, fmt, ap);
    fputc(10, stderr);
    va_end(ap);
}

static int fails, checks;
#include "adapter_match.h"
#include "harden.h"
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

static void test_formula(void)
{
    const uint8_t b[4] = {0x1A, 0xF8, 0x80, 0x05};
    float v;
    CHECK(custom_pid_eval("((A*256)+B)/4", b, 4, &v) && NEAR(v, 1726.0f));
    CHECK(custom_pid_eval("A-40", b, 4, &v) && NEAR(v, -14.0f));
    CHECK(custom_pid_eval("(C-128)*100/128", b, 4, &v) && NEAR(v, 0.0f));
    CHECK(custom_pid_eval("{D:0}", b, 4, &v) && NEAR(v, 1.0f));
    CHECK(custom_pid_eval("{D:1}", b, 4, &v) && NEAR(v, 0.0f));
    CHECK(custom_pid_eval("-A + 2*B", b, 4, &v) && NEAR(v, -26.0f + 496.0f));
    CHECK(custom_pid_eval("A/0", b, 4, &v) && NEAR(v, 0.0f));          /* divide by zero guarded */
    CHECK(!custom_pid_eval("E", b, 4, &v));                               /* missing byte */
    CHECK(!custom_pid_eval("A+", b, 4, &v));                              /* syntax error */
    CHECK(custom_pid_eval("AA", b, 4, &v) == false);                      /* index 26 out of range */
}

static void test_custom_csv(void)
{
    const char *path = "/tmp/carmind_test_pids.csv";
    FILE *f = fopen(path, "w");
    fputs("ModePID,Description,Min,Max,Units,Formula,Header\n"
          "0133,Baro,0,255,kPa,A,0\n"
          "# comment\n"
          "222813,Tyre FL,0,5,bar,(A*256+B)/1000,7E0\n"
          "bad\n", f);
    fclose(f);
    custom_pid_t out[8];
    size_t n = custom_pids_load(path, out, 8);
    CHECK(n == 2);
    CHECK(strcmp(out[0].cmd, "0133") == 0 && strcmp(out[0].prefix, "4133") == 0 && out[0].header[0] == 0);
    CHECK(strcmp(out[1].cmd, "222813") == 0 && strcmp(out[1].prefix, "622813") == 0 && strcmp(out[1].header, "7E0") == 0);
    CHECK(strcmp(out[1].name, "Tyre FL") == 0 && strcmp(out[1].unit, "bar") == 0);
    unlink(path);
    unlink("/tmp/carmind_test_pids.example.csv");
}

static void test_dtc_decode(void)
{
    const uint8_t raw[] = {0x03, 0x00, 0x01, 0x71, 0x42, 0x02, 0xC1, 0x23, 0x00, 0x00};
    char s[64];
    int n = dtc_bytes_to_string(raw, sizeof raw, s, sizeof s);
    CHECK(n == 4);
    CHECK(strcmp(s, "P0300 P0171 C0202 U0123") == 0);
}

static void test_fault_engine(void)
{
    fault_report_t r;
    fault_engine_reset();

    fault_engine_analyse("", "", "", false, &r);
    CHECK(r.n == 0 && r.max_severity == 0);

    /* lean both banks + a misfire pair + evap: expect misfire group first (critical), lean group, evap group */
    fault_engine_analyse("P0171 P0174 P0301 P0303 P0455 P0442", "P0420", "", true, &r);
    CHECK(r.n == 7);
    CHECK(r.mil_on);
    CHECK(r.max_severity == SEV_CRITICAL);
    CHECK(strcmp(r.faults[0].code, "P0301") == 0 || strcmp(r.faults[0].code, "P0303") == 0);
    CHECK(r.faults[0].severity == SEV_CRITICAL && r.faults[0].group >= 0);
    CHECK(strcmp(r.faults[0].desc, "Cylinder 1 misfire detected") == 0 || strcmp(r.faults[0].desc, "Cylinder 3 misfire detected") == 0);
    CHECK(r.ngroups == 3);
    int lean = -1, evap = -1;
    for (int i = 0; i < r.n; i++) {
        if (strcmp(r.faults[i].code, "P0171") == 0) lean = r.faults[i].group;
        if (strcmp(r.faults[i].code, "P0455") == 0) evap = r.faults[i].group;
        if (strcmp(r.faults[i].code, "P0420") == 0) CHECK(r.faults[i].sources == DTC_PENDING);
    }
    CHECK(lean >= 0 && strcmp(r.groups[lean].title, "Lean on both banks") == 0);
    CHECK(evap >= 0 && strcmp(r.groups[evap].cause, "Check the fuel cap first") == 0);
    CHECK(r.new_serious == 2);                 /* the two misfires are new and >= HIGH */
    CHECK(strstr(r.insight, "3 shared causes") != NULL);

    /* second analysis: nothing new, one more pending code appears */
    fault_engine_analyse("P0171 P0174 P0301 P0303 P0455 P0442", "P0420 P0562", "", true, &r);
    CHECK(r.new_serious == 1);                 /* P0562 is HIGH and new */
    for (int i = 0; i < r.n; i++) if (strcmp(r.faults[i].code, "P0301") == 0) CHECK(!r.faults[i].is_new);

    /* fallbacks: manufacturer, chassis, unknown generic range */
    fault_engine_analyse("P1234 C0035 P0612 P0A93", NULL, NULL, false, &r);
    CHECK(r.n == 4);
    for (int i = 0; i < r.n; i++) {
        if (strcmp(r.faults[i].code, "C0035") == 0) CHECK(r.faults[i].severity == SEV_HIGH && strcmp(r.faults[i].subsystem, "chassis") == 0);
        if (strcmp(r.faults[i].code, "P1234") == 0) CHECK(strcmp(r.faults[i].subsystem, "powertrain") == 0);
        if (strcmp(r.faults[i].code, "P0612") == 0) CHECK(strcmp(r.faults[i].subsystem, "ecu") == 0);
        if (strcmp(r.faults[i].code, "P0A93") == 0) CHECK(strcmp(r.faults[i].subsystem, "hybrid") == 0);
    }

    /* verification-agent traps: decimal cylinder/coil numbers, reserved codes, P2 generic, P0524 */
    fault_engine_analyse("P0310 P030A P0362 P2187 P0524", NULL, NULL, false, &r);
    for (int i = 0; i < r.n; i++) {
        if (strcmp(r.faults[i].code, "P0310") == 0) CHECK(strcmp(r.faults[i].desc, "Cylinder 10 misfire detected") == 0);
        if (strcmp(r.faults[i].code, "P030A") == 0) CHECK(strstr(r.faults[i].desc, "Cylinder") == NULL);
        if (strcmp(r.faults[i].code, "P0362") == 0) CHECK(strcmp(r.faults[i].desc, "Ignition coil L circuit") == 0);
        if (strcmp(r.faults[i].code, "P2187") == 0) CHECK(strstr(r.faults[i].desc, "generic") != NULL);
        if (strcmp(r.faults[i].code, "P0524") == 0) CHECK(r.faults[i].severity == SEV_CRITICAL);
    }
    /* single misfire with MIL on becomes critical */
    fault_engine_analyse("P0302", NULL, NULL, true, &r);
    CHECK(r.n == 1 && r.faults[0].severity == SEV_CRITICAL);
    fault_engine_analyse("P0302", NULL, NULL, false, &r);
    CHECK(r.n == 1 && r.faults[0].severity == SEV_HIGH);

    /* low voltage with many codes groups everything */
    fault_engine_analyse("P0562 P0135 P0141 P0500 P0700", NULL, NULL, true, &r);
    CHECK(r.ngroups >= 1 && strcmp(r.groups[0].title, "Low voltage, many codes") == 0);
    CHECK(strcmp(r.faults[0].severity >= SEV_HIGH ? "ok" : "bad", "ok") == 0);
}

static void test_pattern(void)
{
    pattern_model_t m;
    pattern_init(&m);
    pattern_result_t r;
    int closed = 0, spawned = 0;
    (void)spawned;
    int64_t t = 1000;
    /* 40 windows: alternate "idle" and "cruise" regimes, 20 samples per 10 s window */
    for (int wdx = 0; wdx < 40; wdx++) {
        bool cruise = (wdx / 2) % 2;
        for (int i = 0; i < 20; i++) {
            float sig[PATTERN_SIGNALS] = {cruise ? 2500 + i * 5 : 800, cruise ? 90 : 0, cruise ? 35 : 12, cruise ? 18 : 3,
                                          cruise ? 20 : 8, -2, 1, 88, 25, cruise ? 60 : 30};
            t += 520;
            if (pattern_feed(&m, t, sig, 0x3FF, &r)) { closed++; spawned += r.spawned; }
        }
    }
    CHECK(closed >= 38);
    CHECK(m.used >= 2 && m.used <= 6);           /* two regimes should not explode into 24 centroids */
    CHECK(strlen(r.sax_rpm) == PATTERN_SAX_LEN);
    CHECK(r.window_seq == (uint32_t)closed);
    CHECK(r.pattern_id >= 0 && r.pattern_id < m.used);
    CHECK(r.novelty < 1.5f);                     /* a repeated regime is not novel */

    /* round-trip the model through JSON */
    static char buf[12288];
    int n = pattern_model_to_json(&m, buf, sizeof buf);
    CHECK(n > 100);
    pattern_model_t m2;
    CHECK(pattern_model_from_json(&m2, buf));
    CHECK(m2.used == m.used && m2.n_windows == m.n_windows);
    CHECK(fabsf(m2.centroid[0][0] - m.centroid[0][0]) < 1e-3f);
    CHECK(!pattern_model_from_json(&m2, "{\"used\":99}"));
}

static void test_adapter_names(void)
{
    const char *list = "OBDBLE,OBDII,OBD2,OBDLink,ELM327,VEEPEAK,vLinker,V-LINK,IOS-Vlink,Vgate,iCar,Viecar,KONNWEI";
    const char *yes[] = {"OBDBLE", "obdii", "OBD2", "OBDLink MX+", "OBDLink CX", "ELM327 v1.5", "Veepeak OBDCheck BLE",
                         "vLinker MC-IOS", "V-LINK", "IOS-Vlink", "Vgate iCar Pro", "iCar Pro", "Viecar", "KONNWEI KW902"};
    for (size_t i = 0; i < sizeof yes / sizeof *yes; i++) CHECK(adapter_name_matches(yes[i], strlen(yes[i]), list));
    const char *no[] = {"JBL Flip 5", "Galaxy Watch", "Mi Band 6", "", "OB", "LELink"};
    for (size_t i = 0; i < sizeof no / sizeof *no; i++) CHECK(!adapter_name_matches(no[i], strlen(no[i]), list));
    CHECK(adapter_name_matches("OBDIIXYZ", 5, " OBDII "));              /* entries are trimmed */
    CHECK(!adapter_name_matches("OBDIIXYZ", 3, "OBDII"));               /* only `len` bytes of the name count */
    CHECK(adapter_name_matches("abc", 3, ",,  ,abc,"));                 /* empty entries are skipped */
    CHECK(!adapter_name_matches("abc", 3, ""));
    CHECK(!adapter_name_matches(NULL, 0, list));
    CHECK(!adapter_name_matches("abc", 3, NULL));
}

static void test_harden(void)
{
    CHECK(!harden_epoch_plausible(0));
    CHECK(!harden_epoch_plausible(1767225599LL));
    CHECK(harden_epoch_plausible(1767225600LL));
    CHECK(harden_epoch_plausible(1793438962LL));
    CHECK(!harden_epoch_plausible(4102444800LL));
    CHECK(!harden_epoch_plausible(-5));
    CHECK(harden_wifi_cred_ok("Home", ""));
    CHECK(harden_wifi_cred_ok("Home", "12345678"));
    CHECK(!harden_wifi_cred_ok("Home", "1234567"));
    CHECK(!harden_wifi_cred_ok("", "12345678"));
    CHECK(!harden_wifi_cred_ok("0123456789012345678901234567890123", "12345678"));
    CHECK(!harden_wifi_cred_ok("Ho\nme", "12345678"));
    CHECK(!harden_wifi_cred_ok("Home", "pass\rword1"));
    CHECK(!harden_wifi_cred_ok(NULL, ""));
    float a = 3.5f, b = 1e30f, c = -1e30f, d = NAN, e = INFINITY;
    CHECK(harden_model_input(&a) && a == 3.5f);
    CHECK(harden_model_input(&b) && b == 100.0f);
    CHECK(harden_model_input(&c) && c == -100.0f);
    CHECK(!harden_model_input(&d));
    CHECK(!harden_model_input(&e));
    CHECK(harden_timezone_ok("CST6CDT,M3.2.0,M11.1.0"));
    CHECK(harden_timezone_ok("<+06>-6"));
    CHECK(harden_timezone_ok("UTC0"));
    CHECK(!harden_timezone_ok("UT"));
    CHECK(!harden_timezone_ok("CST6;reboot"));
    CHECK(!harden_timezone_ok("CST 6"));
    CHECK(!harden_timezone_ok(NULL));
}

int main(void)
{
    test_harden();
    test_adapter_names();
    test_pattern();
    test_formula();
    test_custom_csv();
    test_dtc_decode();
    test_fault_engine();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
