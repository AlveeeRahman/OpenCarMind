#!/usr/bin/env python3
# Every sys_event()/SYS_EVENT_NET() text must fit the event ring's EVENT_TEXT (events.h) once formatted, or the screen and
# the log cut it mid-word. Worst-case widths per conversion: counts %d/%u 4, %lu/%llu 6 (KB of the 128 MB NAND),
# %s 8 (ECU ids, mode and model names; an esp_err_to_name text needs its own margin, up to 24), %c 1, %.Nf/%f 8, %x 8.  usage: check_event_text.py <main dir>
import re, sys, os
d = sys.argv[1]
limit = int(re.search(r"#define EVENT_TEXT\s+(\d+)", open(os.path.join(d, "events.h")).read()).group(1)) - 1
W = {"d": 4, "i": 4, "u": 4, "ld": 6, "lu": 6, "lld": 6, "llu": 6, "s": 8, "c": 1, "f": 8, "x": 8, "lx": 8, "%": 1}
# S3-shared texts: changing them moves the S3 image, so they wait for the S3 copies of these fixes
EXEMPT = {"Clock sync refused: %s gave %lld, outside 2026-2099"}
bad = 0; seen = 0
for fn in sorted(os.listdir(d)):
    if not fn.endswith((".c", ".cpp")): continue
    src = open(os.path.join(d, fn), errors="replace").read()
    for m in re.finditer(r"\b(sys_event|SYS_EVENT_NET|sys_event_net)\s*\(\s*EV_\w+\s*,\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)", src):
        text = "".join(re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(2))).encode().decode("unicode_escape")
        def width(mm):
            conv = mm.group(2); prec = mm.group(1)
            if conv == "s" and prec and prec.startswith("."): return int(prec[1:])
            return W.get(conv, 8)
        n = len(re.sub(r"%([-+ 0#]*\d*(?:\.\d+)?)(lld|llu|ld|lu|lx|[diusfxc%])", lambda mm: "x" * width(mm), text).encode())
        seen += 1
        if n > limit and text not in EXEMPT:
            bad += 1; line = src[:m.start()].count("\n") + 1
            print(f"{fn}:{line}: {n} > {limit}: {text}")
print(f"event texts checked {seen}, too long {bad}")
sys.exit(1 if bad else 0)
