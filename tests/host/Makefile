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
	rm -f carmind_tests rev_tests tank_tests session_tests
.PHONY: run rev session clean
