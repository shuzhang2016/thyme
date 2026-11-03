#!/usr/bin/env python3
"""CI-10 / DOC-01: check relative Markdown links in first-party docs.

Links to paths outside the repository (sibling repos, monorepo files) are
skipped: they are only valid in a full checkout and cannot be validated here.
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCS = [ROOT / "README.md", ROOT / "ARCHITECTURE.md", *sorted((ROOT / "docs").rglob("*.md"))]
LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
SKIP_PREFIX = ("http://", "https://", "mailto:", "#")


def prose_only(text: str) -> str:
    """Drop fenced code blocks and inline code spans before scanning for links.

    C++ lambdas (`[](void* ctx) { ... }`) and generics otherwise look like
    Markdown links to the regex below.
    """
    out: list[str] = []
    fence = False
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            fence = not fence
            continue
        out.append("" if fence else re.sub(r"`[^`]*`", "", line))
    return "\n".join(out)


errors: list[str] = []

for doc in DOCS:
    text = doc.read_text(encoding="utf-8", errors="replace")
    for raw in LINK.findall(prose_only(text)):
        target = raw.split("#", 1)[0].strip()
        if not target or target.startswith(SKIP_PREFIX):
            continue
        path = (doc.parent / target).resolve()
        try:
            path.relative_to(ROOT)
        except ValueError:
            continue  # sibling repo / monorepo path, not in this checkout
        if not path.exists():
            errors.append(f"{doc.relative_to(ROOT)}: missing link target {target}")

if errors:
    print("doc link check failed:", file=sys.stderr)
    print("\n".join(errors), file=sys.stderr)
    sys.exit(1)
print(f"doc link check OK ({len(DOCS)} files)")
