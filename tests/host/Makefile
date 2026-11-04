CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -O1 -I../../main -Istubs -D_GNU_SOURCE
SRCS = test_main.c ../../main/custom_pids.c ../../main/fault_engine.c ../../main/pattern.c ../../main/adapter_match.c ../../main/econ.c ../../main/harden.c ../../main/power_state.c

run: carmind_tests
	./carmind_tests

carmind_tests: $(SRCS) ../../main/custom_pids.h ../../main/fault_engine.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

# CarMind R.E.V learners (fuel head, airflow constant, calibration, state file) and the fuel source picker
REV_SRCS = test_rev_learn.c ../../main/rev_learn.c ../../main/rev_cal.c ../../main/rev_state.c ../../main/econ.c
rev: rev_tests
	./rev_tests

rev_tests: $(REV_SRCS) ../../main/rev_learn.h ../../main/rev_cal.h ../../main/rev_state.h ../../main/econ.h ../../main/ml_params_rev.h
	$(CC) $(CFLAGS) -o $@ $(REV_SRCS) -lm

clean:
	rm -f carmind_tests rev_tests
.PHONY: run rev clean
