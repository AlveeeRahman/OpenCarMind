CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -O1 -I../../main -Istubs -D_GNU_SOURCE
SRCS = test_main.c ../../main/custom_pids.c ../../main/fault_engine.c ../../main/pattern.c

run: carmind_tests
	./carmind_tests

carmind_tests: $(SRCS) ../../main/custom_pids.h ../../main/fault_engine.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

clean:
	rm -f carmind_tests
.PHONY: run clean
