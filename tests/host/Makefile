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
	rm -f carmind_tests rev_tests tank_tests session_tests reset_tests swipe_tests clock_tests uimetrics_tests prefetch_tests logstore_tests nandfat_tests
.PHONY: run rev session reset swipe clock uimetrics prefetch logstore clean
