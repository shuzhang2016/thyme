#!/usr/bin/env python3
"""CI-07: flake quarantine registry gate.

Reads ``tools/ci/quarantine.json`` and turns the active (non-expired,
non-example) entries into ctest selectors:

* ``--exclude-regex`` prints an anchored regex for ``ctest -E`` — the MR
  gate / branch / nightly hard jobs exclude quarantined tests with it.
* ``--only-regex`` prints an anchored regex for ``ctest -R`` —
  ``nightly-quarantine`` runs exactly the quarantined tests with it, so a
  flaky test keeps generating signal without gating anything.
* ``--list`` prints a human-readable table of the entries.

Both regexes are empty when no entry is active; callers must skip the
``ctest -E``/``-R`` flag entirely in that case (an empty ``-E`` regex would
match every test).

Validation is a hard gate, not a warning: an entry that is expired, missing
``reason``/``expires`` (or any required field), malformed, duplicated, or
naming a test absent from the ``--tests`` list exits 1. Expiry forces a
re-review: either the flake is fixed and the entry leaves the registry, or
the entry is renewed with a fresh reason and expiry. Entries marked
``"example": true`` are documentation templates and ignored entirely.

Exit codes: 0 = registry valid, 1 = validation failure (expired / malformed /
unknown test), 2 = usage or I/O error. ``--self-test`` runs the embedded
unit tests for the validation logic.
"""
from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_REGISTRY = ROOT / "tools" / "ci" / "quarantine.json"
REQUIRED_FIELDS = ("test", "reason", "issue", "quarantined", "expires")

_CTEST_N_LINE = re.compile(r"^\s*Test\s+#\d+:\s+(\S+)\s*$")
_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def load_registry(path: pathlib.Path) -> dict:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise SystemExit(f"{path}: {error}") from error
    except json.JSONDecodeError as error:
        raise SystemExit(f"{path}: invalid JSON: {error}") from error
    if not isinstance(doc, dict) or not isinstance(doc.get("entries"), list):
        raise SystemExit(f"{path}: expected an object with an 'entries' list")
    return doc


def parse_date(value: object) -> datetime.date | None:
    """ISO YYYY-MM-DD, real calendar dates only; None when invalid."""
    if not isinstance(value, str) or not _DATE_RE.match(value):
        return None
    try:
        return datetime.date.fromisoformat(value)
    except ValueError:
        return None


def is_example(entry: object) -> bool:
    return isinstance(entry, dict) and entry.get("example") is True


def entry_errors(entry: object, index: int, today: datetime.date) -> list[str]:
    """Validation errors for one non-example registry entry."""
    where = f"entries[{index}]"
    if not isinstance(entry, dict):
        return [f"{where}: entry is not an object"]
    errors = []
    for field in REQUIRED_FIELDS:
        if not entry.get(field):
            errors.append(f"{where}: missing or empty '{field}'")
    if errors:
        return errors
    name = entry["test"]
    quarantined = parse_date(entry["quarantined"])
    expires = parse_date(entry["expires"])
    if quarantined is None:
        errors.append(f"{where} ({name}): 'quarantined' is not ISO YYYY-MM-DD")
    if expires is None:
        errors.append(f"{where} ({name}): 'expires' is not ISO YYYY-MM-DD")
    if quarantined is not None and expires is not None:
        if expires <= quarantined:
            errors.append(
                f"{where} ({name}): expires {expires} must be after "
                f"quarantined {quarantined}")
        elif expires < today:
            errors.append(
                f"{where} ({name}): EXPIRED {expires} (today {today}) — "
                "fix the flake and drop the entry, or renew it with a fresh "
                "reason/expiry")
    return errors


def validate(registry: dict, today: datetime.date,
             known_tests: set[str] | None = None) -> list[str]:
    """All registry errors; empty list means the registry may be applied."""
    errors: list[str] = []
    seen: dict[str, int] = {}
    for index, entry in enumerate(registry["entries"]):
        if is_example(entry):
            continue
        errors.extend(entry_errors(entry, index, today))
        if isinstance(entry, dict) and entry.get("test"):
            name = entry["test"]
            if name in seen:
                errors.append(
                    f"entries[{index}] ({name}): duplicate of "
                    f"entries[{seen[name]}]")
            seen[name] = index
            if known_tests is not None and name not in known_tests:
                errors.append(
                    f"entries[{index}] ({name}): no such ctest test — "
                    "remove or rename the entry")
    return errors


def active_entries(registry: dict, today: datetime.date) -> list[dict]:
    """Entries that apply right now: not examples, not expired. Call only
    after validate() passed (expired entries are errors, never silently
    applied or silently dropped)."""
    out = []
    for entry in registry["entries"]:
        if is_example(entry):
            continue
        expires = parse_date(entry.get("expires"))
        if expires is not None and expires >= today:
            out.append(entry)
    return out


def build_regex(names: list[str]) -> str:
    """Anchored alternation; empty input -> empty string (callers skip the
    ctest flag entirely: an empty -E regex would exclude everything)."""
    if not names:
        return ""
    return "^(" + "|".join(re.escape(n) for n in sorted(names)) + ")$"


def load_test_list(path: str) -> set[str]:
    """Test names, one per line; ``ctest -N`` output is accepted as-is."""
    try:
        text = sys.stdin.read() if path == "-" else pathlib.Path(
            path).read_text(encoding="utf-8")
    except OSError as error:
        raise SystemExit(f"{path}: {error}") from error
    names = set()
    for line in text.splitlines():
        match = _CTEST_N_LINE.match(line)
        if match:
            names.add(match.group(1))
            continue
        token = line.strip()
        if token and not token.startswith(("Total Tests:", "#")):
            names.add(token.split()[0])
    return names


def self_test() -> int:
    today = datetime.date(2026, 10, 23)
    good = {"test": "test_flaky", "reason": "races under load",
            "issue": "CI-07", "quarantined": "2026-10-20",
            "expires": "2026-11-03"}
    example = dict(good, example=True, test="test_template",
                   quarantined="not-a-date", expires="")

    def registry(*entries):
        return {"schema": 1, "entries": list(entries)}

    def check(cond: bool, label: str) -> bool:
        print(f"{'ok' if cond else 'FAIL'}: {label}")
        return cond

    ok = True
    ok &= check(validate(registry(good), today) == [], "valid entry passes")
    ok &= check(validate(registry(example), today) == [],
                "example entry is ignored even when malformed")
    for field in REQUIRED_FIELDS:
        bad = dict(good)
        bad[field] = ""
        errs = validate(registry(bad), today)
        ok &= check(any(field in e for e in errs), f"missing '{field}' fails")
    expired = dict(good, expires="2026-10-22")
    ok &= check(any("EXPIRED" in e for e in validate(registry(expired), today)),
                "expired entry fails")
    ok &= check(validate(registry(expired), today,
                         {"test_flaky"}) != [],
                "expired fails even with a known test list")
    ok &= check(validate(registry(good), today, {"other_test"}) != [],
                "unknown test name fails")
    ok &= check(validate(registry(good), today, {"test_flaky"}) == [],
                "known test name passes")
    ok &= check(validate(registry(good, good), today) != [],
                "duplicate test fails")
    bad_dates = dict(good, quarantined="2026-02-30")
    ok &= check(validate(registry(bad_dates), today) != [],
                "impossible calendar date fails")
    reversed_dates = dict(good, quarantined="2026-11-03", expires="2026-11-03")
    ok &= check(validate(registry(reversed_dates), today) != [],
                "expires <= quarantined fails")
    not_expired_yet = dict(good, expires="2026-10-23")
    ok &= check(validate(registry(not_expired_yet), today) == [],
                "expiry today is still active (expires at end of day)")
    active = active_entries(registry(good, example), today)
    ok &= check([e["test"] for e in active] == ["test_flaky"],
                "active_entries keeps real entries, drops examples")
    ok &= check(build_regex([]) == "", "empty regex for no entries")
    ok &= check(build_regex(["b.t", "a"]) == r"^(a|b\.t)$",
                "regex is anchored, sorted and escaped")
    return 0 if ok else 1


def print_list(entries: list[dict], today: datetime.date) -> None:
    if not entries:
        print("quarantine: no active entries")
        return
    print(f"{'Test':<40} {'Expires':>10} {'Issue':>10}  Reason")
    print("-" * 80)
    for entry in entries:
        print(f"{entry['test']:<40} {entry['expires']:>10} "
              f"{entry['issue']:>10}  {entry['reason']}")
    print(f"\n{len(entries)} active entr{'ies' if len(entries) != 1 else 'y'}"
          f" (today {today})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--registry", default=str(DEFAULT_REGISTRY),
                        help="quarantine registry (default: "
                             "tools/ci/quarantine.json)")
    parser.add_argument("--tests",
                        help="file with the ctest test list, one name per "
                             "line or raw `ctest -N` output ('-' = stdin); "
                             "entries naming absent tests then fail validation")
    parser.add_argument("--today",
                        help="override today's date (YYYY-MM-DD; testing)")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--exclude-regex", action="store_true",
                      help="print the ctest -E regex for gating jobs")
    mode.add_argument("--only-regex", action="store_true",
                      help="print the ctest -R regex selecting only "
                           "quarantined tests (nightly signal job)")
    mode.add_argument("--list", action="store_true",
                      help="print the active entries as a table")
    mode.add_argument("--self-test", action="store_true",
                      help="run the embedded validation unit tests")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    today = datetime.date.today()
    if args.today:
        today = parse_date(args.today)
        if today is None:
            print(f"invalid --today: {args.today!r}", file=sys.stderr)
            return 2

    registry_path = pathlib.Path(args.registry).resolve()
    registry = load_registry(registry_path)
    known_tests = load_test_list(args.tests) if args.tests else None
    errors = validate(registry, today, known_tests)
    if errors:
        for error in errors:
            print(f"quarantine: {error}", file=sys.stderr)
        return 1

    entries = active_entries(registry, today)
    if args.list:
        print_list(entries, today)
        return 0
    names = [entry["test"] for entry in entries]
    regex = build_regex(names)
    if regex:
        print(regex)
    return 0


if __name__ == "__main__":
    sys.exit(main())
