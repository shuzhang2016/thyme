#!/usr/bin/env python3
"""Channel observability audit for generated thyme graphs.

Reads ``generated/channel_capabilities.json`` (ARCH-19) and answers one
question per edge: *if data is overwritten or a publish is rejected, can the
consumer notice?* Channels whose answer is "only from a writer/reader-side
counter" must be listed in an allowlist with a reason, so a new ``latest`` or
``reject`` channel cannot slip into the graph unnoticed.

Usage:
    python3 channel_audit.py --manifest generated/channel_capabilities.json \
        [--allow config/channel_audit.allow.json] [--matrix out.md] [--json]

Exit codes: 0 = clean, 1 = findings, 2 = usage/manifest error.

The allowlist is JSON::

    {"schema": 1,
     "entries": [{"channel": "can_state", "from": "can_ingress",
                  "to": "can_worker", "reason": "state snapshot; latest is
                  the contract"}]}
"""
from __future__ import annotations

import argparse
import json
import sys

# Why the channel needs an allowlist entry.
RISKS = {
    "latest": "latest: the backend hides intermediate publishes, so the "
              "consumer cannot count skipped frames",
    "reject": "reject/backpressure: the producer gets a failure status; "
              "ignoring it loses the frame",
    "single_slot_pinned": "pinned_slot with slot_count < 2 has no pre-roll "
                          "and writers skip on every in-flight pin",
}


def load_json(path: str):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def allow_key(row: dict, entry: dict) -> bool:
    if entry.get("channel") != row.get("channel"):
        return False
    for field in ("from", "to"):
        if field in entry and entry[field] != row.get(field):
            return False
    return True


def consumer_detection(row: dict) -> tuple[str, str]:
    """(mechanism, metric) the consumer/operator can use for this edge."""
    retention = row.get("retention")
    overflow = row.get("overflow")
    transport = row.get("transport_kind")
    if transport == "pinned_slot":
        return ("writer-only", "pinned_skip / lease_steal + FrameGap")
    if retention == "latest":
        return ("none", "no counter; consumer sees only 'new value'")
    if overflow == "drop":
        return ("reader-drops", "reader_drop_slots / drop_count")
    if overflow in ("reject", "block"):
        return ("writer-status", "Publish status / push_fail_count")
    return ("reader-drops", "reader_drop_slots / drop_count")


def audit(manifest: dict, allow: dict) -> tuple[list[str], list[dict]]:
    errors: list[str] = []
    rows = manifest.get("channels")
    if not isinstance(rows, list):
        return ["manifest: channels must be an array"], []

    entries = allow.get("entries", [])
    if not isinstance(entries, list):
        return ["allowlist: entries must be an array"], []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or not entry.get("channel"):
            errors.append(f"allowlist entries[{index}]: needs a channel")
            continue
        if not str(entry.get("reason", "")).strip():
            errors.append(
                f"allowlist entries[{index}] ({entry['channel']}): needs a reason")
        if not any(allow_key(row, entry) for row in rows):
            errors.append(
                f"allowlist entry '{entry['channel']}' matches no channel "
                "(stale allowlist?)")

    out: list[dict] = []
    for row in rows:
        channel = row.get("channel", "?")
        risky: list[str] = []
        if row.get("retention") == "latest":
            risky.append("latest")
        if row.get("overflow") in ("reject", "block"):
            risky.append("reject")
        if (row.get("transport_kind") == "pinned_slot"
                and int(row.get("slot_count", 0)) < 2):
            risky.append("single_slot_pinned")
        covered = [r for r in risky
                   if any(allow_key(row, entry) for entry in entries)]
        for risk in risky:
            if risk not in covered:
                errors.append(
                    f"{channel} ({row.get('from')}->{row.get('to')}): "
                    f"{RISKS[risk]}; add an allowlist entry with a reason")
        mechanism, metric = consumer_detection(row)
        out.append({
            "channel": channel,
            "from": row.get("from"),
            "to": row.get("to"),
            "transport": row.get("transport_kind"),
            "retention": row.get("retention"),
            "overflow": row.get("overflow"),
            "consumer_detects": mechanism,
            "metric": metric,
            "allowlisted": bool(risky) and len(covered) == len(risky),
        })
    return errors, out


def matrix_markdown(rows: list[dict]) -> str:
    lines = [
        "| channel | from -> to | transport | retention | overflow | "
        "consumer detects | metric |",
        "|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        lines.append(
            f"| {r['channel']} | {r['from']} -> {r['to']} | {r['transport']} "
            f"| {r['retention']} | {r['overflow']} | {r['consumer_detects']} "
            f"| {r['metric']} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", required=True,
                   help="generated/channel_capabilities.json")
    p.add_argument("--allow", help="allowlist JSON (see module docstring)")
    p.add_argument("--matrix", help="write the observability matrix here")
    p.add_argument("--json", action="store_true",
                   help="print the matrix as JSON instead of Markdown")
    args = p.parse_args(argv)

    try:
        manifest = load_json(args.manifest)
        allow = load_json(args.allow) if args.allow else {"entries": []}
    except (OSError, ValueError) as exc:
        print(f"channel_audit: {exc}", file=sys.stderr)
        return 2

    errors, rows = audit(manifest, allow)
    for err in errors:
        print(f"channel_audit: {err}", file=sys.stderr)

    text = (json.dumps(rows, indent=2) + "\n") if args.json \
        else matrix_markdown(rows)
    if args.matrix:
        with open(args.matrix, "w", encoding="utf-8") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
