# libFuzzer run over the firmware's input parsers. `make` builds and fuzzes for RUNS iterations (CI uses a short
# run on every push; raise RUNS or use -max_total_time locally). Needs clang; gcc has no -fsanitize=fuzzer.
CC ?= clang
RUNS ?= 20000
M = ../../main
CFLAGS = -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -I$(M) -I$(M)/obd -I$(M)/analysis -I$(M)/system -I$(M)/storage -I../host/stubs -D_GNU_SOURCE
SRCS = fuzz_parsers.c $(M)/system/harden.c $(M)/obd/custom_pids.c $(M)/obd/adapter_match.c $(M)/analysis/fault_engine.c

run: fuzz_parsers
	mkdir -p corpus
	./fuzz_parsers corpus seeds -runs=$(RUNS) -max_len=256

fuzz_parsers: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

clean:
	rm -rf fuzz_parsers corpus crash-* leak-* timeout-*
.PHONY: run clean
