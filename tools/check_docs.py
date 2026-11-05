#!/usr/bin/env python3
"""Fail if a Markdown file links to a local file or image that does not exist, or if a shipped model's export record
(ml/onboard/export.json, ml/onboard/rev/export.json) does not match its files or is not named in README.md and docs/MODEL.md."""
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)\)|<img[^>]+src=\"([^\"]+)\"")


def check_models():
    problems = []
    docs = (ROOT / "README.md").read_text() + (ROOT / "docs" / "MODEL.md").read_text()
    for export in sorted((ROOT / "ml" / "onboard").rglob("export.json")):
        meta = json.loads(export.read_text())
        where = export.relative_to(ROOT)
        for name, want in meta.get("files", {}).items():
            f = export.parent / name
            if not f.exists():
                problems.append(f"{where}: {name} is listed but missing")
                continue
            data = f.read_bytes()
            if len(data) != want["bytes"] or hashlib.sha256(data).hexdigest() != want["sha256"]:
                problems.append(f"{where}: {name} does not match its size or sha256")
        if meta.get("model") and meta["model"] not in docs:
            problems.append(f"{where}: model name {meta['model']!r} is not in README.md or docs/MODEL.md")
    return problems


def main():
    missing = check_models()
    for doc in sorted(ROOT.rglob("*.md")):
        if ".git" in doc.parts:
            continue
        for match in LINK.finditer(doc.read_text()):
            target = (match.group(1) or match.group(2)).split("#")[0]
            if not target or re.match(r"[a-z]+:", target):
                continue
            if not (doc.parent / target).exists():
                missing.append(f"{doc.relative_to(ROOT)}: {target}")
    for line in missing:
        print("problem:" if ".json:" in line else "missing:", line)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
