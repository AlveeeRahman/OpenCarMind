#!/usr/bin/env python3
"""Read the result of `sphinx-build -b linkcheck` and decide what fails.

    python3 docs-src/check_links.py docs-src/_build/linkcheck/output.json

A broken link inside the site or to a file in this repository fails (exit 1). A broken outside link is listed as a
warning and does not fail, because outside sites go down and block automated requests on their own schedule.
In GitHub Actions the warnings also show as annotations.
"""
import json
import os
import sys
from collections import Counter
from pathlib import Path


def main(path):
    rows = [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]
    counts = Counter(r["status"] for r in rows)
    internal, external = [], []
    for r in rows:
        if r["status"] != "broken":
            continue
        where = f"{r['filename']}:{r.get('lineno', '?')}: {r['uri']} ({r.get('info', '')})"
        (external if r["uri"].startswith(("http://", "https://")) else internal).append(where)
    gha = os.environ.get("GITHUB_ACTIONS") == "true"
    for w in external:
        print(f"::warning title=Outside link broken::{w}" if gha else f"warning: outside link broken: {w}")
    for e in internal:
        print(f"::error title=Link inside the site broken::{e}" if gha else f"error: link inside the site broken: {e}")
    print("linkcheck:", ", ".join(f"{k} {v}" for k, v in sorted(counts.items())),
          f"| inside broken {len(internal)}, outside broken {len(external)}")
    return 1 if internal else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "docs-src/_build/linkcheck/output.json"))
