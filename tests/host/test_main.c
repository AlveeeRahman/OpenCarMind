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
#include "econ.h"
#include "harden.h"
#include "power_state.h"
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

    const char *obd[] = {"OBDBLE", "OBDLink CX", "Veepeak OBDCheck BLE+", "IOS-Vlink", "vLinker MC-IOS", "Carista",
                         "KIWI 3", "LELink2", "Android-Vlink", "KW902", "my ELM327 v2.1", "Viecar VP11"};
    for (size_t i = 0; i < sizeof obd / sizeof *obd; i++) CHECK(adapter_name_looks_obd(obd[i], strlen(obd[i])));
    const char *other[] = {"JBL Flip 5", "Galaxy Watch", "Mi Band 6", "", "HMSoft"};
    for (size_t i = 0; i < sizeof other / sizeof *other; i++) CHECK(!adapter_name_looks_obd(other[i], strlen(other[i])));
    CHECK(!adapter_name_looks_obd("xxOBD", 3));                          /* only `len` bytes count */
    CHECK(!adapter_name_looks_obd(NULL, 0));
}

static adapter_chr_t chr_of(const char *svc, const char *chr, uint8_t props)
{
    adapter_chr_t c;
    CHECK(adapter_uuid_parse(svc, &c.svc));
    CHECK(adapter_uuid_parse(chr, &c.chr));
    c.props = props;
    return c;
}

static void test_adapter_profiles(void)
{
    adapter_uuid_t a, b;
    CHECK(adapter_uuid_parse("fff0", &a));
    b = adapter_uuid16(0xFFF0);
    CHECK(adapter_uuid_eq(&a, &b));
    CHECK(adapter_uuid_parse("0000FFF0-0000-1000-8000-00805F9B34FB", &a) && adapter_uuid_eq(&a, &b));
    CHECK(!adapter_uuid_parse("FFF", &a));
    CHECK(!adapter_uuid_parse("FFFG", &a));
    CHECK(adapter_uuid_parse("E7810A71-73AE-499D-8C15-FAA9AEF0C3F2", &a) && a.b[0] == 0xE7 && a.b[15] == 0xF2);

    /* which advertised services count as an OBD serial line */
    const char *adv_yes[] = {"FFF0", "18F0", "BEEF", "E7810A71-73AE-499D-8C15-FAA9AEF0C3F2"};
    for (size_t i = 0; i < sizeof adv_yes / sizeof *adv_yes; i++) CHECK(adapter_uuid_parse(adv_yes[i], &a) && adapter_service_is_obd(&a));
    const char *adv_no[] = {"FFE0", "180F", "1800", "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"};
    for (size_t i = 0; i < sizeof adv_no / sizeof *adv_no; i++) CHECK(adapter_uuid_parse(adv_no[i], &a) && !adapter_service_is_obd(&a));

    const uint8_t N = ADAPTER_PROP_NOTIFY, I = ADAPTER_PROP_INDICATE, W = ADAPTER_PROP_WRITE, WN = ADAPTER_PROP_WRITE_NR,
                  R = 0x02;
    int ni, wi, prof;

    /* Veepeak / OBDLink CX / Carista: FFF1 notify, FFF2 write */
    adapter_chr_t fff[] = {chr_of("180A", "2A29", R), chr_of("FFF0", "FFF1", N), chr_of("FFF0", "FFF2", W | WN)};
    CHECK(adapter_choose(fff, 3, "FFE0", "FFE1", "FFE1", &ni, &wi, &prof) && ni == 1 && wi == 2);
    CHECK(strcmp(adapter_profile_name(prof), "FFF0") == 0);

    /* LELink: one characteristic does both; the configured profile wins */
    adapter_chr_t ffe[] = {chr_of("FFE0", "FFE1", N | W | WN)};
    CHECK(adapter_choose(ffe, 1, "FFE0", "FFE1", "FFE1", &ni, &wi, &prof) && ni == 0 && wi == 0 && prof == -2);

    /* Deep OBD: FFE1 notify, FFE2 write */
    adapter_chr_t deep[] = {chr_of("FFE0", "FFE1", N), chr_of("FFE0", "FFE2", W)};
    CHECK(adapter_choose(deep, 2, "", "", "", &ni, &wi, &prof) && ni == 0 && wi == 1);
    CHECK(strcmp(adapter_profile_name(prof), "FFE0 split") == 0);

    /* Vgate vLinker: one 128-bit characteristic */
    adapter_chr_t vl[] = {chr_of("1801", "2A05", I), chr_of("E7810A71-73AE-499D-8C15-FAA9AEF0C3F2",
                                                           "BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F", N | W)};
    CHECK(adapter_choose(vl, 2, "FFE0", "FFE1", "FFE1", &ni, &wi, &prof) && ni == 1 && wi == 1);
    CHECK(strcmp(adapter_profile_name(prof), "Vgate E7810A71") == 0);

    /* Vgate iCar Pro BLE: 18F0 must not be mistaken for a SIG service */
    adapter_chr_t icar[] = {chr_of("18F0", "2AF0", N), chr_of("18F0", "2AF1", WN)};
    CHECK(adapter_choose(icar, 2, "FFE0", "FFE1", "FFE1", &ni, &wi, &prof) && ni == 0 && wi == 1);

    /* unknown vendor service with indicate only: generic fallback, standard services skipped */
    adapter_chr_t gen[] = {chr_of("180F", "2A19", N | R), chr_of("1801", "2A05", I),
                           chr_of("49535343-FE7D-4AE5-8FA9-9FAFD205E455", "49535343-1E4D-4BD9-BA61-23C647249616", I),
                           chr_of("49535343-FE7D-4AE5-8FA9-9FAFD205E455", "49535343-8841-43F4-A8D4-ECBE34729BB3", W)};
    CHECK(adapter_choose(gen, 4, "FFE0", "FFE1", "FFE1", &ni, &wi, &prof) && ni == 2 && wi == 3 && prof == -1);

    /* BEEF is preferred over another vendor service that comes first */
    adapter_chr_t beef[] = {chr_of("AE30", "AE01", W), chr_of("AE30", "AE02", N),
                            chr_of("BEEF", "BEE1", N), chr_of("BEEF", "BEE2", W)};
    CHECK(adapter_choose(beef, 4, "", "", "", &ni, &wi, &prof) && ni == 2 && wi == 3);

    /* a known profile whose notify characteristic lost its CCCD (props cleared) falls through */
    adapter_chr_t nocccd[] = {chr_of("FFF0", "FFF1", 0), chr_of("FFF0", "FFF2", W)};
    CHECK(!adapter_choose(nocccd, 2, "", "", "", &ni, &wi, &prof));

    /* nothing usable */
    adapter_chr_t none[] = {chr_of("180A", "2A29", R), chr_of("180F", "2A19", N)};
    CHECK(!adapter_choose(none, 2, "FFE0", "FFE1", "FFE1", &ni, &wi, &prof));
    CHECK(!adapter_choose(NULL, 0, "", "", "", &ni, &wi, &prof));
}


static bool near(double a, double b, double tol) { return a > b - tol && a < b + tol; }

static void test_econ(void)
{
    /* units from where the board is: Wi-Fi country first, time zone only for the unambiguous UK */
    CHECK(units_from_country("US") == UNITS_US);
    CHECK(units_from_country("us") == UNITS_US);
    CHECK(units_from_country("PR") == UNITS_US);
    CHECK(units_from_country("GB") == UNITS_UK);
    CHECK(units_from_country("DE") == UNITS_METRIC);
    CHECK(units_from_country("CA") == UNITS_METRIC);
    CHECK(units_from_country("01") == -1);                 /* world-safe code: says nothing */
    CHECK(units_from_country("") == -1);
    CHECK(units_from_country(NULL) == -1);
    CHECK(units_from_timezone("GMT0BST,M3.5.0/1,M10.5.0") == UNITS_UK);
    CHECK(units_from_timezone("EST5EDT,M3.2.0,M11.1.0") == -1);   /* shared by the US and Canada */
    CHECK(units_from_timezone("") == -1);
    CHECK(units_parse("us") == UNITS_US && units_parse("metric") == UNITS_METRIC && units_parse("uk") == UNITS_UK);
    CHECK(units_parse("auto") == -1 && units_parse(NULL) == -1);
    CHECK(!strcmp(units_econ_label(UNITS_METRIC), "km/L") && !strcmp(units_econ_label(UNITS_US), "mpg"));
    CHECK(!strcmp(units_dist_label(UNITS_UK), "mi"));

    /* fuel rate: car PID, then MAF with the commanded ratio, then the model, then nothing */
    fuel_src_t src;
    CHECK(near(econ_fuel_lph(true, 2.5f, true, 9.0f, true, 1.0f, 1, true, 3.0f, &src), 2.5, 1e-6) && src == FUEL_PID);
    CHECK(near(econ_fuel_lph(false, 0, true, 4.44f, true, 1.0f, 1, true, 3.0f, &src), 1.4595, 0.001) && src == FUEL_MAF);
    CHECK(near(econ_fuel_lph(false, 0, true, 4.44f, false, 0, 0, false, 0, &src), 1.4595, 0.001) && src == FUEL_MAF);
    CHECK(econ_fuel_lph(false, 0, true, 4.44f, true, 0.0f, 1, false, 0, &src) == 0 && src == FUEL_MAF);   /* fuel cut */
    CHECK(near(econ_fuel_lph(false, 0, true, 4.44f, true, 0.912f, 1, false, 0, &src), 1.4595 / 0.912, 0.002));   /* enriched */
    CHECK(near(econ_fuel_lph(false, 0, true, 4.44f, true, 7.0f, 1, false, 0, &src), 1.4595, 0.001));   /* nonsense ratio: 1.0 */
    /* diesels run lean: a commanded 3.0 is real, so 14.5 g/s of air at lambda 3 is 1/3 g/s of fuel: 1/3*3600/832 L/h */
    CHECK(near(econ_fuel_lph(false, 0, true, 14.5f, true, 3.0f, 4, false, 0, &src), 3600.0 / 832.0 / 3.0, 0.001) && src == FUEL_MAF);
    CHECK(near(econ_fuel_lph(false, 0, true, 14.5f, true, 9.0f, 4, false, 0, &src), 3600.0 / 832.0, 0.001));   /* past 8: 1.0 */
    CHECK(near(econ_fuel_lph(false, 0, true, 14.7f, true, 3.0f, 1, false, 0, &src), 3600.0 / 745.0, 0.001));   /* petrol at 3: 1.0 */
    /* a stale fuel cut is passed as lambda unknown by the caller: stoichiometric, not zero */
    CHECK(near(econ_fuel_lph(false, 0, true, 14.7f, false, 0.0f, 1, false, 0, &src), 3600.0 / 745.0, 0.001));
    CHECK(near(econ_fuel_lph(false, 0, true, 10.0f, false, 0, 4, false, 0, &src), 10.0 / 14.5 * 3600 / 832, 0.001));  /* diesel */
    CHECK(near(econ_fuel_lph(false, 0, false, 0, false, 0, 0, true, 1.3f, &src), 1.3, 1e-6) && src == FUEL_MODEL);
    CHECK(econ_fuel_lph(false, 0, false, 0, false, 0, 0, false, 0, &src) < 0 && src == FUEL_NONE);   /* old car, no MAF */
    CHECK(econ_fuel_lph(true, -1, false, 0, false, 0, 0, false, 0, &src) < 0 && src == FUEL_NONE);
    CHECK(econ_fuel_lph(false, 0, false, 0, false, 0, 0, true, 250.0f, &src) < 0);                    /* absurd model value */
    CHECK(!strcmp(fuel_src_name(FUEL_MAF), "maf") && !strcmp(fuel_src_name(FUEL_NONE), ""));

    /* trip: 30 km/h for an hour at 2.5 L/h is 30 km on 2.5 L = 12 km/L = 28.2 mpg US = 33.9 mpg UK */
    econ_trip_t t; econ_reset(&t);
    CHECK(econ_trip_kpl(&t) < 0);
    for (int i = 0; i < 3600; i++) econ_step(&t, 30, 2.5f, 1.0f);
    CHECK(near(t.km, 30, 1e-6) && near(t.litres, 2.5, 1e-6));
    CHECK(near(econ_trip_kpl(&t), 12, 1e-4));
    CHECK(near(econ_to_unit(12, UNITS_US), 28.225, 0.01));
    CHECK(near(econ_to_unit(12, UNITS_UK), 33.896, 0.01));
    CHECK(near(econ_to_unit(12, UNITS_METRIC), 12, 1e-6));
    CHECK(econ_to_unit(-1, UNITS_US) < 0);
    CHECK(near(econ_dist_to_unit(16.09344, UNITS_US), 10, 1e-6));
    econ_step(&t, 30, -1, 1.0f);   CHECK(near(t.km, 30, 1e-6));   /* unknown fuel: the row does not count */
    econ_step(&t, 30, 2.5f, 60);   CHECK(near(t.km, 30, 1e-6));   /* a long gap is not driving */
    econ_step(&t, -1, 2.5f, 1);    CHECK(near(t.km, 30, 1e-6));

    /* instant economy */
    CHECK(econ_instant_kpl(0, 1.0f) < 0);
    CHECK(econ_instant_kpl(4, 1.0f) < 0);
    CHECK(near(econ_instant_kpl(50, 2.5f), 20, 1e-4));
    CHECK(near(econ_instant_kpl(50, 0.0f), 99.9, 1e-3));          /* coasting on fuel cut */
    CHECK(econ_instant_kpl(50, -1) < 0);
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

/* Simulates a drive from start to switch-off: the screen may say "Safe to turn off" only at the end. */
static void test_power_state(void)
{
    CHECK(power_state(false, true, 99999, false, false) == POWER_NO_CARD);
    CHECK(power_state(true, true, 99999, true, false) == POWER_LOGGING);          /* car link up */
    CHECK(power_state(true, true, 1200, false, false) == POWER_LOGGING);          /* rows still fresh, model still running */
    CHECK(power_state(true, true, 4999, false, false) == POWER_LOGGING);
    CHECK(power_state(true, false, 5000, false, false) == POWER_SAVING);          /* data stopped, last save pending */
    CHECK(power_state(true, true, 5000, false, true) == POWER_UPLOADING);
    CHECK(power_state(true, true, 5000, false, false) == POWER_SAFE);             /* stale data, saved, idle */
    CHECK(power_state(true, true, INT64_MAX, false, false) == POWER_SAFE);        /* parked since boot */
    /* a timeline: rows every 1.5 s, then the car stops at 30 s; saves every 10 s */
    int safe_at = -1;
    for (int ms = 0; ms <= 60000; ms += 500) {
        int64_t last_row = ms <= 30000 ? (ms / 1500) * 1500 : 30000;
        bool saved = (ms >= 40000);                                   /* first save after the last row */
        power_state_t s = power_state(true, saved, ms - last_row, ms <= 30000, false);
        if (s == POWER_SAFE && safe_at < 0) safe_at = ms;
        if (ms <= 30000) CHECK(s != POWER_SAFE);
    }
    CHECK(safe_at == 40000);
    for (int s = POWER_SAFE; s <= POWER_UPLOADING; s++) CHECK(strlen(power_state_text((power_state_t)s)) <= 18);   /* fits page 3 */
}

int main(void)
{
    test_power_state();
    test_harden();
    test_adapter_names();
    test_adapter_profiles();
    test_econ();
    test_pattern();
    test_formula();
    test_custom_csv();
    test_dtc_decode();
    test_fault_engine();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
