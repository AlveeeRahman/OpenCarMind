#!/usr/bin/env python3
"""Check documentation prose against the house style before it is published.

    python3 docs-src/check_prose.py FILE.md ...          check these pages
    python3 docs-src/check_prose.py --changed BASE        check the docs-src pages changed since git ref BASE
    python3 docs-src/check_prose.py --all                 check every hand-written page in docs-src

Two sets of rules run on each page. The repository's own: the product is always "OpenCarMind EdgeOS", never
"EdgeOS" alone, and no private details (local paths, Bluetooth or Wi-Fi addresses, a VIN). Then the humanizer
style check (tools/check_draft.py --style-only): no em dashes, no semicolons, no banned words, varied openings.
MyST markup (directive fences and options, targets, attribute lines) is blanked first, so only prose is checked.
Exit code 1 when any page has an error. Warnings are printed and do not fail.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
CHECKER = HERE / "tools" / "check_draft.py"
GENERATED = ("_gen/", "manuals/", "release-notes/", "reference/c-api/", "_build/", "tools/")

MYST_LINE = re.compile(r"^\s*(:{3,}|`{3,}\{|:[\w-]+:|\{\.[\w-]+\}\s*$|\([\w.-]+\)=\s*$|---\s*$|html_theme\.)")
EDGEOS_ALONE = re.compile(r"(?<!OpenCarMind )\bEdge ?OS\b(?!-)")
PRIVATE = [
    (re.compile(r"/Users/|/home/\w|/private/tmp|C:\\\\Users"), "a local path"),
    (re.compile(r"\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b"), "a Bluetooth or Wi-Fi address"),
    (re.compile(r"\b[A-HJ-NPR-Z0-9]{17}\b"), "something shaped like a VIN"),
    (re.compile(r"\bA\.E\.V\b|\btest R\.E\.V\b"), "an unreleased model name"),
]


def masked(text):
    """The page with MyST markup lines and code blocks blanked, keeping the line numbers."""
    out, fence = [], None
    for line in text.split("\n"):
        stripped = line.strip()
        if fence:
            out.append("")
            if stripped.startswith(fence):
                fence = None
            continue
        m = re.match(r"^(`{3,}|~{3,})", stripped)
        if m:
            fence = m.group(1)
            out.append("")
            continue
        line = re.sub(r"&(?:[a-z]+|#\d+);", "x", line)        # an HTML entity's ; is not a semicolon
        out.append("" if MYST_LINE.match(line) else line)
    return "\n".join(out)


def check(path):
    text = path.read_text(encoding="utf-8")
    prose = masked(text)
    errors = []
    for n, line in enumerate(prose.split("\n"), 1):
        bare = re.sub(r"`[^`]*`|\]\([^)]*\)", "", line)
        if EDGEOS_ALONE.search(bare):
            errors.append(f"{path}:{n}: ERROR  write \"OpenCarMind EdgeOS\", not \"EdgeOS\" alone")
        for rx, what in PRIVATE:
            if rx.search(line):
                errors.append(f"{path}:{n}: ERROR  {what}")
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8") as tmp:
        tmp.write(prose)
    res = subprocess.run([sys.executable, str(CHECKER), tmp.name, "--style-only"], capture_output=True, text=True)
    Path(tmp.name).unlink()
    warns = []
    for line in res.stdout.splitlines():
        m = re.match(r"^(ERROR|WARN)\s+line\s+(\d+)\s+(.*)$", line.strip())
        if m:
            item = f"{path}:{m.group(2)}: {m.group(1):5}  {m.group(3)}"
            (errors if m.group(1) == "ERROR" else warns).append(item)
    return errors, warns


def pages(args):
    if args.all:
        return sorted(p for p in HERE.rglob("*.md")
                      if not str(p.relative_to(HERE)).startswith(GENERATED) and p.name != "README.md")
    if args.changed:
        out = subprocess.run(["git", "diff", "--name-only", "--diff-filter=AM", f"{args.changed}...HEAD", "--",
                              "docs-src"], capture_output=True, text=True, cwd=REPO, check=True).stdout.split()
        return [REPO / p for p in out if p.endswith(".md")
                and not p[len("docs-src/"):].startswith(GENERATED) and not p.endswith("README.md")]
    return [Path(p).resolve() for p in args.files]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--changed", metavar="BASE")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    errors, warns = [], []
    files = pages(args)
    for p in files:
        e, w = check(p)
        errors += e
        warns += w
    for line in warns + errors:
        print(line.replace(str(REPO) + "/", ""))
    print(f"{len(files)} pages checked, {len(errors)} errors, {len(warns)} warnings")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
