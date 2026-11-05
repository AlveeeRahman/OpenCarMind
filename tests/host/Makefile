CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -O1 -I../../main -I../../main/obd -I../../main/model -I../../main/analysis -I../../main/system -I../../main/storage -Istubs -D_GNU_SOURCE
SRCS = test_main.c ../../main/obd/custom_pids.c ../../main/analysis/fault_engine.c ../../main/analysis/pattern.c ../../main/obd/adapter_match.c ../../main/analysis/econ.c ../../main/obd/obd_fresh.c ../../main/system/harden.c ../../main/system/power_state.c

run: carmind_tests
	./carmind_tests

carmind_tests: $(SRCS) ../../main/obd/custom_pids.h ../../main/analysis/fault_engine.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

# CarMind R.E.V learners (fuel head, airflow constant, calibration, state file) and the fuel source picker
REV_SRCS = test_rev_learn.c ../../main/model/rev_learn.c ../../main/model/rev_torque.c ../../main/model/rev_tank.c ../../main/model/rev_cal.c ../../main/model/rev_state.c ../../main/analysis/econ.c
rev: rev_tests tank_tests
	./rev_tests
	./tank_tests

tank_tests: test_rev_tank.c ../../main/model/rev_tank.c ../../main/model/rev_tank.h
	$(CC) $(CFLAGS) -o $@ test_rev_tank.c ../../main/model/rev_tank.c -lm

rev_tests: $(REV_SRCS) ../../main/model/rev_learn.h ../../main/model/rev_cal.h ../../main/model/rev_state.h ../../main/analysis/econ.h ../../main/model/ml_params_rev.h
	$(CC) $(CFLAGS) -o $@ $(REV_SRCS) -lm

clean:
	rm -f carmind_tests rev_tests tank_tests
.PHONY: run rev clean
