CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -O1 -I../../main -Istubs -D_GNU_SOURCE
SRCS = test_main.c ../../main/custom_pids.c ../../main/fault_engine.c ../../main/pattern.c ../../main/adapter_match.c ../../main/econ.c ../../main/obd_fresh.c ../../main/harden.c ../../main/power_state.c ../../main/ecu_select.c

run: carmind_tests
	./carmind_tests

# flash FAT: which FAT copy wins after a reset tore a write (main/fat_mirror.c)
fat: fat_tests
	./fat_tests

fat_tests: test_fat_mirror.c ../../main/fat_mirror.c ../../main/fat_mirror.h
	$(CC) $(CFLAGS) -o $@ test_fat_mirror.c ../../main/fat_mirror.c

# session number after a re-flash erased the NVS counter (main/session_next.c)
session: session_tests
	./session_tests

session_tests: test_session_next.c ../../main/session_next.c ../../main/session_next.h
	$(CC) $(CFLAGS) -o $@ test_session_next.c ../../main/session_next.c

# ESP-Mosaico drive store (F1): file routing, the session counter over two stores, the move to the NAND, drives.csv merge
logstore: logstore_tests nandfat_tests
	./logstore_tests
	./nandfat_tests

# the flash FAT's repair rules on the NAND volume (FAT16, 2 KB sectors) after a Dhara rollback to any prefix of the writes
nandfat_tests: test_nand_fat.c ../../main/fat_mirror.c ../../main/fat_mirror.h
	$(CC) $(CFLAGS) -o $@ test_nand_fat.c ../../main/fat_mirror.c

logstore_tests: test_log_store.c ../../main/log_store.c ../../main/log_store.h ../../main/session_next.c
	$(CC) $(CFLAGS) -o $@ test_log_store.c ../../main/log_store.c ../../main/session_next.c

# which resets count toward the boot-loop safe mode (main/reset_class.c)
reset: reset_tests
	./reset_tests

reset_tests: test_reset_class.c ../../main/reset_class.c ../../main/reset_class.h
	$(CC) $(CFLAGS) -o $@ test_reset_class.c ../../main/reset_class.c

# smart swipe: the OS-ELM swipe-intent learner, its histograms and the A/B gate (main/swipe_intent.c)
swipe: swipe_tests
	./swipe_tests

# Settings > Clock on the ESP-Mosaico: date checks, the time-zone list, the clock-source rule (main/clock_set.c)
clock: clock_tests
	./clock_tests

clock_tests: test_clock_set.c ../../main/clock_set.c ../../main/clock_set.h ../../main/harden.c
	$(CC) $(CFLAGS) -o $@ test_clock_set.c ../../main/clock_set.c ../../main/harden.c -lm

# Smart UI metrics: lift -> settle histograms, the rank test, Holm, the settle detector, the A/B gate (main/ui_metrics.c)
uimetrics: uimetrics_tests
	./uimetrics_tests

uimetrics_tests: test_ui_metrics.c ../../main/ui_metrics.c ../../main/ui_metrics.h
	$(CC) $(CFLAGS) -O2 -o $@ test_ui_metrics.c ../../main/ui_metrics.c -lm

# Smart UI arm A1: when the press-time prefetch builds, commits and cancels (main/ui_prefetch.c)
prefetch: prefetch_tests
	./prefetch_tests

prefetch_tests: test_ui_prefetch.c ../../main/ui_prefetch.c ../../main/ui_prefetch.h
	$(CC) $(CFLAGS) -o $@ test_ui_prefetch.c ../../main/ui_prefetch.c

swipe_tests: test_swipe_intent.c ../../main/swipe_intent.c ../../main/swipe_intent.h ../../main/ui_metrics.c ../../main/ui_metrics.h
	$(CC) $(CFLAGS) -O2 -o $@ test_swipe_intent.c ../../main/ui_metrics.c -lm
# ESP-Mosaico Settings: the blob migration (Wi-Fi app, no app hiding), order moves, change rules, the save debounce
settings: settings_tests
	./settings_tests

settings_tests: test_ui_settings.c ../../main/ui_settings_logic.c ../../main/ui_settings_logic.h
	$(CC) $(CFLAGS) -o $@ test_ui_settings.c ../../main/ui_settings_logic.c

# ESP-Mosaico Wi-Fi app: the search list merge, the password rules (WIFI-ADD's), the join result texts (main/wifi_ui_logic.c)
wifi: wifi_tests
	./wifi_tests

wifi_tests: test_wifi_ui.c ../../main/wifi_ui_logic.c ../../main/wifi_ui_logic.h ../../main/harden.c
	$(CC) $(CFLAGS) -o $@ test_wifi_ui.c ../../main/wifi_ui_logic.c ../../main/harden.c -lm
# ESP-Mosaico recent events (Summary app): times, colours by level, the save line, filters, UTF-8 truncation (T1)
events: events_tests
	./events_tests

events_tests: test_ui_events.c ../../main/ui_events_logic.c ../../main/ui_events_logic.h ../../main/power_state.c
	$(CC) $(CFLAGS) -o $@ test_ui_events.c ../../main/ui_events_logic.c ../../main/power_state.c

# wifi_creds.c itself (copied to a scratch dir so its includes find stubs/wifi_creds): the near-duplicate cleanup when its
# argument points into the list (S31; the S3 copy of the fix comes later)
wificreds: wificreds_tests
	./wificreds_tests

wificreds_tests: test_wifi_creds.c ../../main/wifi_creds.c ../../main/wifi_creds.h ../../main/harden.c stubs/wifi_creds/*.h
	mkdir -p wificreds_src && cp ../../main/wifi_creds.c wificreds_src/
	$(CC) $(CFLAGS:-I../../main=-Istubs/wifi_creds -I../../main) -o $@ test_wifi_creds.c wificreds_src/wifi_creds.c ../../main/harden.c

# app lists updated in place: which rows an update writes, hides or creates (main/ui_rows_logic.c)
# every sys_event() text fits the event ring (EVENT_TEXT) once formatted, so the screen and the log never cut it
eventtext:
	python3 check_event_text.py ../../main

rows: rows_tests
	./rows_tests

rows_tests: test_ui_rows.c ../../main/ui_rows_logic.c ../../main/ui_rows_logic.h
	$(CC) $(CFLAGS) -o $@ test_ui_rows.c ../../main/ui_rows_logic.c

# ESP-Mosaico palettes: every text/ground pair >= 4.5:1, graphics >= 3:1, read from mosaico_display.c (T1)
theme: theme_tests
	./theme_tests

theme_tests: test_ui_theme.c ../../main/mosaico_display.c
	$(CC) $(CFLAGS) -o $@ test_ui_theme.c -lm

# ESP-Mosaico PPA and 2D-DMA: slide split, aligned re-basing, PSRAM regions, encryption rows, fallback (main/ocm_ppa_geom.c)
ppa: ppa_tests
	./ppa_tests

ppa_tests: test_ppa_geom.c ../../main/ocm_ppa_geom.c ../../main/ocm_ppa_geom.h
	$(CC) $(CFLAGS) -O2 -o $@ test_ppa_geom.c ../../main/ocm_ppa_geom.c

# mossREV OS layer (2.0.0): the pure rules (main/os_core.c) plus a source audit (os_audit.sh)
os: os_tests
	./os_tests
	./os_audit.sh

os_tests: test_os.c ../../main/os_core.c ../../main/os_core.h ../../main/reset_class.c
	$(CC) $(CFLAGS) -o $@ test_os.c ../../main/os_core.c ../../main/reset_class.c -lm

# Bluetooth OBD adapter choice and pairing ("ask + strong signal"): the pure rules (main/obd_pair.c) plus a source audit
# rules, and is_candidate() of obd_ble.c with CONFIG_OBD_BLE_PAIR_CONFIRM off (the car board's tested rule) and on
pair: pair_tests pair_gate_tests
	./pair_tests
	./pair_gate_tests
	./pair_audit.sh

pair_tests: test_obd_pair.c ../../main/obd_pair.c ../../main/obd_pair.h
	$(CC) $(CFLAGS) -o $@ test_obd_pair.c ../../main/obd_pair.c

# unifdef exits 1 when it removed something
pair_gate_tests: test_pair_gate.c ../../main/obd_ble.c ../../main/obd_pair.c ../../main/adapter_match.c
	unifdef -DCONFIG_OBD_BLE_PAIR_CONFIRM=0 ../../main/obd_ble.c | \
	    awk '/^static bool is_candidate\(/,/^}/' | sed 's/is_candidate(/is_candidate_off(/' > pair_gate_off.inc || [ $$? = 1 ]
	unifdef -DCONFIG_OBD_BLE_PAIR_CONFIRM=1 ../../main/obd_ble.c | \
	    awk '/^static void addr_key\(/,/^}/; /^static obd_pick_t is_candidate\(/,/^}/' | \
	    sed 's/is_candidate(/is_candidate_on(/' > pair_gate_on.inc || [ $$? = 1 ]
	grep -q is_candidate_off pair_gate_off.inc && grep -q is_candidate_on pair_gate_on.inc
	$(CC) $(CFLAGS) -I. -o $@ test_pair_gate.c ../../main/obd_pair.c ../../main/adapter_match.c
	rm -f pair_gate_off.inc pair_gate_on.inc

# pairing safety on the ESP-Mosaico: the adapter fingerprint (R1), the re-pair table (R2), the guard question
# (main/adapter_guard.c), plus a source audit of the firmware glue (guard_audit.sh)
guard: guard_tests
	./guard_tests
	./guard_audit.sh
guard_tests: test_adapter_guard.c ../../main/adapter_guard.c ../../main/adapter_guard.h
	$(CC) $(CFLAGS) -o $@ test_adapter_guard.c ../../main/adapter_guard.c
carmind_tests: $(SRCS) ../../main/custom_pids.h ../../main/fault_engine.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

# CarMind R.E.V learners (fuel head, airflow constant, calibration, state file) and the fuel source picker
REV_SRCS = test_rev_learn.c ../../main/rev_learn.c ../../main/rev_torque.c ../../main/rev_tank.c ../../main/rev_cal.c ../../main/rev_state.c ../../main/econ.c
rev: rev_tests tank_tests
	./rev_tests
	./tank_tests

tank_tests: test_rev_tank.c ../../main/rev_tank.c ../../main/rev_tank.h
	$(CC) $(CFLAGS) -o $@ test_rev_tank.c ../../main/rev_tank.c -lm

rev_tests: $(REV_SRCS) ../../main/rev_learn.h ../../main/rev_cal.h ../../main/rev_state.h ../../main/econ.h ../../main/ml_params_rev.h
	$(CC) $(CFLAGS) -o $@ $(REV_SRCS) -lm

# EdgeOS AI services: the release table under overload, the degrade order against the OS ladder, the seqlock and ring
# (main/ai_svc_core.c, main/ai_snap.h)
ai: ai_tests
	./ai_tests

ai_tests: test_ai_svc.c ../../main/ai_svc_core.c ../../main/ai_svc_core.h ../../main/ai_snap.h ../../main/os_core.c ../../main/os_core.h
	$(CC) $(CFLAGS) -o $@ test_ai_svc.c ../../main/ai_svc_core.c ../../main/os_core.c -lm

# Battery SoC/SoH estimator (main/bat_est_core.c with main/bat_model.h). After the research agent's bat_model.h lands in
# main/, rerun: make -C tests/host bat. The C port is checked against its test_vectors.csv to 1e-4 (BAT_VECTORS).
# A header not yet in main/: make -C tests/host bat BAT_MODEL=/path/to/bat_model.h
BAT_DIR ?= $(HOME)/Desktop/dev/carmind-ml/experiments/battery_soc
BAT_VECTORS ?= $(BAT_DIR)/test_vectors.csv
BAT_MODEL ?=
BAT_MODEL_DEF = $(if $(BAT_MODEL),-DBAT_MODEL_FILE='"$(BAT_MODEL)"',)
bat: bat_tests
	BAT_VECTORS="$(BAT_VECTORS)" ./bat_tests

bat_tests: test_bat_est.c ../../main/bat_est_core.c ../../main/bat_est_core.h ../../main/bat_model.h $(BAT_MODEL) FORCE
	$(CC) $(CFLAGS) -O2 $(BAT_MODEL_DEF) -o $@ test_bat_est.c ../../main/bat_est_core.c -lm

# OpenCarMind EdgeOS 2.1.0 AI services. The research vectors live in carmind-ml (never in this tree):
#   soh   the SoH ridge (main/bat_soh_model.h) against soh_vectors.csv, the Q(V) features against p3_soh.py (vec/), the cycle logic
#   anom  the battery anomaly detector (main/bat_anom_core.c) against anom_vectors.csv, plus the 2 s sample period
#   ids   the network guard: the rotation detector bit-exact against ids_vectors.csv, the guard's policy, the hook bitmaps
#   idsaudit  the radio hooks only count and set bits; the guard never transmits, pairs or sets the clock (ids_audit.sh)
SOH_VECTORS ?= $(BAT_DIR)/soh_vectors.csv
ANOM_VECTORS ?= $(BAT_DIR)/anom_vectors.csv
IDS_VECTORS ?= $(HOME)/Desktop/dev/carmind-ml/experiments/net_ids/tests/ids_vectors.csv
soh: soh_tests
	SOH_VECTORS="$(SOH_VECTORS)" ./soh_tests

soh_tests: test_bat_soh.c ../../main/bat_soh.c ../../main/bat_soh.h ../../main/bat_soh_model.h ../../main/bat_est_core.h
	$(CC) $(CFLAGS) -O2 -o $@ test_bat_soh.c ../../main/bat_soh.c -lm

anom: anom_tests
	ANOM_VECTORS="$(ANOM_VECTORS)" ./anom_tests

anom_tests: test_bat_anom.c ../../main/bat_anom_core.c ../../main/bat_anom_core.h ../../main/bat_anom_model.h
	$(CC) $(CFLAGS) -O2 -o $@ test_bat_anom.c ../../main/bat_anom_core.c -lm

ids: ids_tests idsaudit
	IDS_VECTORS="$(IDS_VECTORS)" ./ids_tests

ids_tests: test_ids.c ../../main/ids_core.c ../../main/ids_core.h ../../main/ids_hooks.h ../../main/ids_model.h
	$(CC) $(CFLAGS) -O2 -o $@ test_ids.c ../../main/ids_core.c

idsaudit:
	./ids_audit.sh

FORCE:

clean:
	rm -f carmind_tests rev_tests tank_tests session_tests reset_tests swipe_tests clock_tests uimetrics_tests prefetch_tests logstore_tests nandfat_tests settings_tests wifi_tests events_tests theme_tests wificreds_tests rows_tests ppa_tests os_tests pair_tests pair_gate_tests pair_gate_off.inc pair_gate_on.inc uishot_bin ai_tests bat_tests guard_tests soh_tests anom_tests ids_tests
	rm -rf uishot_bin.dSYM
.PHONY: run rev session reset swipe clock uimetrics prefetch logstore clean settings wifi events theme wificreds rows eventtext ppa os pair textfit uishot uishot_lvgl ai bat guard soh anom ids idsaudit FORCE

# ESP-Mosaico screens rendered on the Mac: text fit, corners, the 16 px floor (uishot/uishot.mk)
include uishot/uishot.mk
