#!/usr/bin/env python3
"""Tests for channel_audit.py (OBS-15 channel observability gate)."""
from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import channel_audit  # noqa: E402


def row(channel="can_state", retention="latest", overflow="overwrite",
        transport="frame", slot_count=0, src="a", dst="b"):
    return {
        "channel": channel, "from": src, "to": dst, "shm": "",
        "transport_kind": transport, "retention": retention,
        "readers": "one", "overflow": overflow, "recordable": True,
        "replay_gate": False, "lease_required": transport == "pinned_slot",
        "hub_required": False, "max_wire_bytes": 128,
        "slot_count": slot_count,
    }


def manifest(*rows):
    return {"version": 1, "hash": "x", "channels": list(rows)}


class AuditTests(unittest.TestCase):
    def test_unlisted_latest_fails(self):
        errors, rows = channel_audit.audit(manifest(row()), {"entries": []})
        self.assertTrue(any("latest" in e for e in errors))
        self.assertEqual(rows[0]["consumer_detects"], "none")

    def test_allowlisted_latest_passes(self):
        allow = {"entries": [{"channel": "can_state", "reason": "state"}]}
        errors, rows = channel_audit.audit(manifest(row()), allow)
        self.assertEqual(errors, [])
        self.assertTrue(rows[0]["allowlisted"])

    def test_allowlist_field_must_match(self):
        allow = {"entries": [{"channel": "can_state", "to": "other",
                              "reason": "state"}]}
        errors, _ = channel_audit.audit(manifest(row()), allow)
        self.assertTrue(any("matches no channel" in e for e in errors))
        self.assertTrue(any("latest" in e for e in errors))

    def test_allowlist_needs_reason(self):
        allow = {"entries": [{"channel": "can_state", "reason": "  "}]}
        errors, _ = channel_audit.audit(manifest(row()), allow)
        self.assertTrue(any("needs a reason" in e for e in errors))

    def test_reject_requires_entry_and_reports_push_fail(self):
        r = row(retention="queue", overflow="reject")
        errors, rows = channel_audit.audit(manifest(r), {"entries": []})
        self.assertTrue(any("reject" in e for e in errors))
        self.assertEqual(rows[0]["consumer_detects"], "writer-status")

    def test_legacy_block_name_is_still_risky(self):
        r = row(retention="queue", overflow="block")
        errors, rows = channel_audit.audit(manifest(r), {"entries": []})
        self.assertTrue(any("reject" in e for e in errors))
        self.assertEqual(rows[0]["consumer_detects"], "writer-status")

    def test_drop_channel_is_reader_observable(self):
        r = row(retention="queue", overflow="drop")
        errors, rows = channel_audit.audit(manifest(r), {"entries": []})
        self.assertEqual(errors, [])
        self.assertEqual(rows[0]["consumer_detects"], "reader-drops")

    def test_single_slot_pinned_fails(self):
        r = row(retention="queue", overflow="drop", transport="pinned_slot",
                slot_count=1)
        errors, rows = channel_audit.audit(manifest(r), {"entries": []})
        self.assertTrue(any("slot_count < 2" in e for e in errors))
        self.assertEqual(rows[0]["consumer_detects"], "writer-only")

    def test_two_slot_pinned_is_fine(self):
        r = row(retention="queue", overflow="drop", transport="pinned_slot",
                slot_count=2)
        errors, _ = channel_audit.audit(manifest(r), {"entries": []})
        self.assertEqual(errors, [])

    def test_matrix_contains_every_channel(self):
        r1 = row()
        r2 = row(channel="estop", retention="queue", overflow="reject")
        _, rows = channel_audit.audit(manifest(r1, r2), {"entries": []})
        md = channel_audit.matrix_markdown(rows)
        self.assertIn("| can_state |", md)
        self.assertIn("| estop |", md)
        self.assertEqual(md.count("\n"), 4)  # header + separator + 2 rows

    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            man = pathlib.Path(tmp) / "m.json"
            man.write_text(json.dumps(manifest(row())))
            code = channel_audit.main(["--manifest", str(man)])
            self.assertEqual(code, 1)
            allow = pathlib.Path(tmp) / "a.json"
            allow.write_text(json.dumps(
                {"schema": 1, "entries": [{"channel": "can_state",
                                           "reason": "state"}]}))
            out = pathlib.Path(tmp) / "matrix.md"
            code = channel_audit.main(["--manifest", str(man),
                                       "--allow", str(allow),
                                       "--matrix", str(out)])
            self.assertEqual(code, 0)
            self.assertIn("can_state", out.read_text())


if __name__ == "__main__":
    unittest.main()
