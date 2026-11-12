#!/usr/bin/env python3
# Every sys_event()/SYS_EVENT_NET() text must fit the event ring's EVENT_TEXT (events.h) once formatted, or the screen and
# the log cut it mid-word. Worst-case widths per conversion: counts %d/%u 4, %lu/%llu 6 (KB of the 128 MB NAND), %c 1,
# %.Nf/%f 8, %x 8. %s takes the real width of its argument where the source shows it (2.1.0): a string literal or a
# ternary of literals (the longest), a call to a function in main/ whose body returns only string literals (the
# longest of them: os_ladder_name, ids_reason_text, ...), a char array declared in the same file (its size - 1),
# esp_err_to_name 24; anything else 8 (ECU ids, mode and model names).  usage: check_event_text.py <main dir>
import re, sys, os
d = sys.argv[1]
limit = int(re.search(r"#define EVENT_TEXT\s+(\d+)", open(os.path.join(d, "events.h")).read()).group(1)) - 1
W = {"d": 4, "i": 4, "u": 4, "ld": 6, "lu": 6, "lld": 6, "llu": 6, "s": 8, "c": 1, "f": 8, "x": 8, "lx": 8, "%": 1}
# S3-shared texts: changing them moves the S3 image, so they wait for the S3 copies of these fixes
EXEMPT = {"Clock sync refused: %s gave %lld, outside 2026-2099"}
SRC = {fn: open(os.path.join(d, fn), errors="replace").read() for fn in os.listdir(d) if fn.endswith((".c", ".cpp", ".h"))}
ALL = "\n".join(SRC.values())
LIT = r"\"((?:[^\"\\]|\\.)*)\""
FIXED = {"esp_err_to_name": 24}

def split_args(t):
    out, depth, cur, q = [], 0, "", False
    for i, ch in enumerate(t):
        if q:
            cur += ch
            if ch == '"' and t[i - 1] != "\\": q = False
            continue
        if ch == '"': q = True; cur += ch; continue
        if ch in "([{": depth += 1
        if ch in ")]}":
            if depth == 0: out.append(cur); return out
            depth -= 1
        if ch == "," and depth == 0: out.append(cur); cur = ""; continue
        cur += ch
    out.append(cur)
    return out

def func_width(name):
    if name in FIXED: return FIXED[name]
    m = re.search(r"\b(?:const\s+)?char\s*\*\s*" + re.escape(name) + r"\s*\([^;{)]*\)\s*\{(.*?)\n\}", ALL, re.S)
    if not m: return None
    lits = re.findall(LIT, m.group(1))
    return max((len(x.encode()) for x in lits), default=None)

def arg_width(a, src):
    a = a.strip()
    lits = re.fullmatch(r"(?:[^\"]*\?)?\s*(" + LIT + r")\s*:\s*(" + LIT + r")\s*", a)
    if re.fullmatch(LIT, a): return len(re.fullmatch(LIT, a).group(1).encode())
    if "?" in a and ":" in a:
        parts = re.findall(LIT, a)
        rest = re.sub(LIT, "", a)
        if parts and len(parts) >= 2 and not re.search(r"\w+\s*\(", rest.split("?", 1)[1]): return max(len(x.encode()) for x in parts)
    m = re.fullmatch(r"(\w+)\s*\(.*\)", a, re.S)
    if m:
        w = func_width(m.group(1))
        if w is not None: return w
    m = re.fullmatch(r"\w+", a)
    if m:
        dm = re.search(r"\bchar\s+" + re.escape(a) + r"\s*\[\s*(\d+)\s*\]", src)
        if dm: return int(dm.group(1)) - 1
    return None

bad = 0; seen = 0
for fn in sorted(os.listdir(d)):
    if not fn.endswith((".c", ".cpp")): continue
    src = open(os.path.join(d, fn), errors="replace").read()
    for m in re.finditer(r"\b(sys_event|SYS_EVENT_NET|sys_event_net)\s*\(\s*EV_\w+\s*,\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)", src):
        text = "".join(re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(2))).encode().decode("unicode_escape")
        args = split_args(src[m.end():].lstrip().lstrip(",")) if src[m.end():].lstrip().startswith(",") else []
        convs = re.findall(r"%([-+ 0#]*\d*(?:\.\d+)?)(lld|llu|ld|lu|lx|[diusfxc%])", text)
        argi = {}
        k = 0
        for ci, (pr, cv) in enumerate(convs):
            if cv == "%": continue
            if "*" in pr: k += 1
            argi[ci] = k; k += 1
        ctr = iter(range(len(convs)))
        def width(mm):
            ci = next(ctr)
            conv = mm.group(2); prec = mm.group(1)
            if conv == "s" and prec and prec.startswith("."): return int(prec[1:])
            if conv in ("x", "lx") and prec.lstrip("-+ #").startswith("0") and prec.lstrip("-+ #0").isdigit():
                return int(prec.lstrip("-+ #0"))                # %04x: a 16-bit hash prefix, 4 wide
            if conv == "s" and ci in argi and argi[ci] < len(args):
                w = arg_width(args[argi[ci]], src)
                if w is not None: return w
            return W.get(conv, 8)
        n = len(re.sub(r"%([-+ 0#]*\d*(?:\.\d+)?)(lld|llu|ld|lu|lx|[diusfxc%])", lambda mm: "x" * width(mm), text).encode())
        seen += 1
        if n > limit and text not in EXEMPT:
            bad += 1; line = src[:m.start()].count("\n") + 1
            print(f"{fn}:{line}: {n} > {limit}: {text}")
print(f"event texts checked {seen}, too long {bad}")
sys.exit(1 if bad else 0)
