#!/usr/bin/env python3
"""Worst-case stack of the long-running ESP-Mosaico tasks, from GCC's call graph (OpenCarMind EdgeOS 2.1.1).

Build with CONFIG_OCM_STACK_USAGE=y (main is compiled with -fstack-usage -fcallgraph-info=su), then:

    tools/stack_check.py <build_dir>

For each task below it adds up the frames of the deepest call path inside main (the .ci files) and adds a fixed
allowance for what main calls in ESP-IDF, the C library and the BLE host, which have no .ci file (printf with floats,
the VFS and FatFs path to the NAND, a NimBLE write). It fails when the sum comes closer than MARGIN to the task's stack.
The field crash of 2.1.0 (a core dump: "stack overflow in task obd") was obd_task 5,104 B + pattern_model_from_json
4,016 B = 9,120 B on an 8,192 B stack, before any library frame; this check fails on that build.

Recursion is cut at the first repeat (custom_pid_eval's parser is bounded by the formula's nesting, at most 8 rows of
a few dozen characters). Indirect calls (function pointers) are not followed: the AI services and LVGL callbacks run
in their own tasks and are checked by their stack watermarks on the board.
"""
import functools
import glob
import re
import sys

# task entry, stack bytes (xTaskCreate), allowance for frames outside main
TASKS = [
    ("obd_task", 8192, 2048),     # elm327.c, OBD_STACK
    ("drive_task", 6144, 2048),   # mosaico_drive.c: the stop's file closes and syncs run here
    ("board_task", 6144, 2048),   # mosaico_board.c
]
MARGIN = 512


def load(build):
    size, edges = {}, {}

    def short(t):
        return t.rsplit(":", 1)[-1] if t.startswith("/") else t

    for f in glob.glob(build + "/**/__idf_main.dir/**/*.ci", recursive=True):
        for line in open(f, errors="replace"):
            m = re.match(r'node: \{ title: "([^"]+)" label: "([^"]*)"', line)
            if m:
                s = re.search(r"\\n(\d+) bytes \(", m.group(2))
                if s:
                    size[short(m.group(1))] = int(s.group(1))
                continue
            m = re.match(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', line)
            if m:
                edges.setdefault(short(m.group(1)), set()).add(short(m.group(2)))
    return size, edges


def worst_path(size, edges, root):
    @functools.lru_cache(None)
    def walk(f, seen):
        best = (0, ())
        for g in edges.get(f, ()):
            if g in seen:
                continue
            w = walk(g, seen | frozenset([g]))
            if w[0] > best[0]:
                best = w
        return (size.get(f, 0) + best[0], (f,) + best[1])

    return walk(root, frozenset([root]))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    size, edges = load(sys.argv[1])
    if not size:
        print("no .ci files: build with CONFIG_OCM_STACK_USAGE=y")
        return 2
    bad = 0
    for root, stack, extra in TASKS:
        if root not in size:
            print("%-11s not found" % root)
            bad += 1
            continue
        total, path = worst_path(size, edges, root)
        need = total + extra
        ok = need + MARGIN <= stack
        bad += not ok
        print("%-11s %5d B in main + %d B allowance = %5d of %5d B: %s" % (root, total, extra, need, stack, "ok" if ok else "TOO CLOSE"))
        print("            " + " > ".join("%s(%s)" % (p, size.get(p, "?")) for p in path))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
