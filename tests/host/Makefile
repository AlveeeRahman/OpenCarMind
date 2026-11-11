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

clean:
	rm -f carmind_tests rev_tests tank_tests session_tests reset_tests swipe_tests clock_tests uimetrics_tests prefetch_tests logstore_tests nandfat_tests settings_tests wifi_tests events_tests theme_tests wificreds_tests rows_tests ppa_tests os_tests
.PHONY: run rev session reset swipe clock uimetrics prefetch logstore clean settings wifi events theme wificreds rows eventtext ppa os
