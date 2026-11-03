#!/usr/bin/env python3
"""Fail if a Markdown file links to a local file or image that does not exist."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)\)|<img[^>]+src=\"([^\"]+)\"")


def main():
    missing = []
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
        print("missing:", line)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
