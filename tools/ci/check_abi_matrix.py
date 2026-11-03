#!/usr/bin/env python3
"""ABI-07: keep the ARCHITECTURE.md ABI matrix in sync with the headers.

The matrix in ARCHITECTURE.md is the short, component-oriented view of the
rules in ``docs/reference/abi-compatibility.md``. Every version is read from the header
that defines it (the same constants ``src/probe/abi_matrix.cpp`` compiles in),
so a version bump that misses the table fails here instead of shipping a
stale document.

The table is delimited by HTML comments and has a fixed six-column shape::

    | Component | Object | Constant | Version | Surface | Rule |

Rows whose object has no numeric version (the perf log has a magic and block
types, no version word) use ``—`` in both Constant and Version.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BEGIN = "<!-- abi-matrix:begin -->"
END = "<!-- abi-matrix:end -->"

# Constant name -> (owning header, regex with one numeric capture group).
SOURCES = {
    "kMpSessionIdentityVersion": (
        "include/thyme/runtime/mp_session_identity_page.h",
        r"\bkMpSessionIdentityVersion\s*=\s*(\d+)",
    ),
    "kSessionLeaseVersion": (
        "include/thyme/runtime/session_lease.h",
        r"\bkSessionLeaseVersion\s*=\s*(\d+)",
    ),
    "kMpControlAbiVersion": (
        "include/thyme/app/mp_control.h",
        r"\bkMpControlAbiVersion\s*=\s*(\d+)",
    ),
    "SharedHealthPage::kVersion": (
        "include/thyme/runtime/shared_health.h",
        r"\bkVersion\s*=\s*(\d+)",
    ),
    "kPinnedSlotStoreVersion": (
        "include/thyme/runtime/pinned_slot_store.h",
        r"\bkPinnedSlotStoreVersion\s*=\s*(\d+)",
    ),
    "kPinnedSlotLeaseVersion": (
        "include/thyme/runtime/pinned_slot_lease.h",
        r"\bkPinnedSlotLeaseVersion\s*=\s*(\d+)",
    ),
    "kShmSpscRingVersion": (
        "include/thyme/runtime/shm_spsc_ring.h",
        r"\bkShmSpscRingVersion\s*=\s*(\d+)",
    ),
    "kShmSpmcRingVersion": (
        "include/thyme/runtime/shm_spmc_ring.h",
        r"\bkShmSpmcRingVersion\s*=\s*(\d+)",
    ),
    "kLatestSlotLayoutVersion": (
        "include/thyme/runtime/latest_slot.h",
        r"\bkLatestSlotLayoutVersion\s*=\s*(\d+)",
    ),
    "RecordMirrorPage::kVersion": (
        "include/thyme/runtime/record_mirror.h",
        r"\bkVersion\s*=\s*(\d+)",
    ),
    "RecordMirrorRingPage::kVersion": (
        "include/thyme/runtime/record_mirror_ring.h",
        r"\bkVersion\s*=\s*(\d+)",
    ),
    "kThymeBinFileAbiVersion": (
        "include/thyme/record/thyme_bin.h",
        r"\bkThymeBinFileAbiVersion\s*=\s*(\d+)",
    ),
    "kThymeBinIndexVersion": (
        "include/thyme/record/thyme_bin.h",
        r"\bkThymeBinIndexVersion\s*=\s*(\d+)",
    ),
    "kTriggerVersion": (
        "include/thyme/record/trigger_protocol.h",
        r"\bkTriggerVersion\s*=\s*(\d+)",
    ),
    "kManifestVersion": (
        "include/thyme/record/package_integrity.h",
        r"\bkManifestVersion\s*=\s*(\d+)",
    ),
}

# Objects the compact matrix must cover. Keeping this list here (rather than
# deriving it from the table) makes deleting a row a check failure, not a
# silent loss of coverage.
REQUIRED_OBJECTS = (
    "MpControl",
    "MpSessionIdentityPage",
    "SessionLeasePage",
    "SharedHealthPage",
    "PinnedSlotPageHeader",
    "PinnedSlotLeaseHeader",
    "ShmSpscRing",
    "ShmSpmcRing",
    "LatestSlot<T>",
    "RecordMirrorPage",
    "RecordMirrorRingPage",
    ".thyme.bin",
    ".thyme.bin footer index",
    "TriggerRequest/TriggerResponse",
    "integrity.manifest",
    "perf log",
)

COLUMNS = ("component", "object", "constant", "version", "surface", "rule")


def split_row(line: str) -> list[str]:
    return [cell.replace("`", "").strip()
            for cell in line.strip().strip("|").split("|")]


def parse_matrix(text: str, doc_name: str) -> tuple[list[dict], list[str]]:
    errors: list[str] = []
    if BEGIN not in text or END not in text:
        return [], [f"{doc_name}: missing {BEGIN} / {END} markers"]
    body = text.split(BEGIN, 1)[1].split(END, 1)[0]
    rows: list[dict] = []
    header_seen = False
    for line_no, line in enumerate(body.splitlines(), 1):
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = split_row(stripped)
        if not header_seen:
            header_seen = True
            if tuple(cell.lower() for cell in cells) != COLUMNS:
                errors.append(
                    f"{doc_name}: matrix header must be "
                    f"'| {' | '.join(c.title() for c in COLUMNS)} |'")
            continue
        if all(set(cell) <= {"-", ":"} for cell in cells if cell):
            continue
        if len(cells) != len(COLUMNS):
            errors.append(
                f"{doc_name}: matrix row has {len(cells)} columns, "
                f"expected {len(COLUMNS)}: {stripped}")
            continue
        row = dict(zip(COLUMNS, cells))
        row["line"] = line_no
        if row["version"] == "—":
            if row["constant"] != "—":
                errors.append(
                    f"{doc_name}:{line_no}: row without a version must use "
                    f"'—' in the Constant column too")
        else:
            try:
                row["version"] = int(row["version"])
            except ValueError:
                errors.append(
                    f"{doc_name}:{line_no}: version '{row['version']}' is not "
                    f"an integer")
                continue
        rows.append(row)
    if not rows:
        errors.append(f"{doc_name}: ABI matrix table is empty")
    return rows, errors


def header_version(root: pathlib.Path, constant: str) -> tuple[int | None, str]:
    rel, pattern = SOURCES[constant]
    path = root / rel
    if not path.is_file():
        return None, f"{rel}: header not found"
    match = re.search(pattern, path.read_text(encoding="utf-8", errors="replace"))
    if match is None:
        return None, f"{rel}: constant {constant} not found"
    return int(match.group(1)), ""


def golden_versions(path: pathlib.Path) -> dict[str, int]:
    if not path.is_file():
        return {}
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}
    return {
        entry.get("constant", ""): entry.get("version")
        for entry in doc.get("entries", [])
        if isinstance(entry.get("version"), int)
    }


def check(root: pathlib.Path, doc: pathlib.Path,
          golden: pathlib.Path | None) -> tuple[list[str], int]:
    errors: list[str] = []
    try:
        text = doc.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"{doc}: cannot read ({exc})"], 0
    rows, parse_errors = parse_matrix(text, doc.relative_to(root).as_posix())
    errors.extend(parse_errors)

    by_object: dict[str, dict] = {}
    checked = 0
    for row in rows:
        if row["object"] in by_object:
            errors.append(f"{doc}: duplicate object row '{row['object']}'")
        by_object[row["object"]] = row
        if row["constant"] == "—":
            continue
        if row["constant"] not in SOURCES:
            errors.append(
                f"{doc}:{row['line']}: unknown ABI constant "
                f"'{row['constant']}' (add it to SOURCES)")
            continue
        actual, error = header_version(root, row["constant"])
        if error:
            errors.append(f"{doc}:{row['line']}: {error}")
            continue
        checked += 1
        if actual != row["version"]:
            errors.append(
                f"{doc}:{row['line']}: {row['object']} says v{row['version']}, "
                f"{row['constant']} in {SOURCES[row['constant']][0]} is v{actual}")

    for obj in REQUIRED_OBJECTS:
        if obj not in by_object:
            errors.append(f"{doc}: matrix is missing required row '{obj}'")

    if golden is not None:
        known = golden_versions(golden)
        for row in rows:
            if row["constant"] == "—" or row["constant"] not in known:
                continue
            if row["version"] != known[row["constant"]]:
                errors.append(
                    f"{doc}:{row['line']}: {row['object']} says "
                    f"v{row['version']}, {golden.name} says "
                    f"v{known[row['constant']]}")

    return errors, checked


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Check the ARCHITECTURE.md ABI matrix against the headers")
    parser.add_argument("--root", default=str(ROOT),
                        help="repository root (default: this checkout)")
    parser.add_argument("--doc", default="ARCHITECTURE.md",
                        help="matrix document, relative to --root")
    parser.add_argument("--golden",
                        default="tools/ops/upgrade/abi_matrix.golden.json",
                        help="OPS-06 JSON matrix to cross-check")
    parser.add_argument("--no-golden", action="store_true",
                        help="skip the JSON matrix cross-check")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    doc = root / args.doc
    golden = None if args.no_golden else root / args.golden
    errors, checked = check(root, doc, golden)
    if errors:
        print("ABI matrix check failed:", file=sys.stderr)
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"ABI matrix check OK ({checked} header constants, "
          f"{len(REQUIRED_OBJECTS)} required rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
