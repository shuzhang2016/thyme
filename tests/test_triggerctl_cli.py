#!/usr/bin/env python3
"""ECO-02 CLI contract tests for triggerctl.

Covers the documented surface without needing a live recorder:

* usage/help and exit-code 2 for argument errors;
* exit-code 5 when no control socket exists (unreachable);
* the request/response `status`/`status-json` protocol against a fake
  AF_UNIX datagram server, including text and JSON output shapes;
* arm/disarm delivery and the JSON acknowledgement;
* the `verify --json` shape for a missing package (exit 1).

The binary path is passed as argv[1] by the CMake test registration.
"""

from __future__ import annotations

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

import zlib
from pathlib import Path

BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None

STATUS_JSON = json.dumps({
    "schema": 1,
    "kind": "thyme-recorder-status",
    "ok": True,
    "socket": "/tmp/fake.sock",
    "armed": False,
    "phase": "post",
    "next_trigger_id": 7,
    "auth_rejects": 2,
    "captures_done": 3,
    "slices_done": 1,
    "pending_bytes": 4096,
    "disk_full": True,
}, separators=(",", ":"))

STATUS_TEXT = ("socket=/tmp/fake.sock armed=false phase=post "
               "next_trigger_id=7 auth_rejects=2 captures_done=3 slices_done=1 "
               "pending_bytes=4096 disk_full=true")


class FakeControlServer(threading.Thread):
    """One-shot AF_UNIX datagram server: records the request, optionally replies."""

    def __init__(self, path: str, reply: str | None = None, code: int = 0,
                 respond: bool = True) -> None:
        super().__init__(daemon=True)
        self.path = path
        self.reply = reply
        self.code = code
        self.respond = respond
        self.received = ""
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(path)
        self.sock.settimeout(5.0)
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        try:
            data, addr = self.sock.recvfrom(4096)
            magic, version, op, sequence, crc, reserved = struct.unpack("<IHHQII", data)
            names = {1: "trigger", 2: "arm", 3: "disarm", 4: "status", 5: "status-json"}
            self.received = names[op]
            if magic != 0x4C544352 or version != 2 or reserved != 0:
                raise ValueError("bad record_control request")
            if zlib.crc32(data[:16]) != crc:
                raise ValueError("bad record_control request CRC")
            if self.respond:
                payload = (self.reply or "").encode()
                header = struct.pack("<IHHQI", magic, version, self.code,
                                     sequence, len(payload))
                crc = zlib.crc32(header + payload)
                self.sock.sendto(header + struct.pack("<I", crc) + payload, addr)
        except (OSError, ValueError):
            pass
        finally:
            self.sock.close()

    def __enter__(self) -> "FakeControlServer":
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._thread.join(timeout=6.0)


def cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(BINARY), *args], capture_output=True, text=True, check=False)


class UsageTests(unittest.TestCase):
    def test_help_exits_zero(self) -> None:
        proc = cli("--help")
        self.assertEqual(proc.returncode, 0)
        self.assertIn("usage: triggerctl", proc.stdout)
        self.assertIn("exit codes", proc.stdout)

    def test_missing_command_is_usage_error(self) -> None:
        proc = cli()
        self.assertEqual(proc.returncode, 2)
        self.assertIn("missing command", proc.stderr)

    def test_unknown_option_is_usage_error(self) -> None:
        proc = cli("status", "--bogus")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("unknown option", proc.stderr)

    def test_missing_option_value_is_usage_error(self) -> None:
        proc = cli("status", "--timeout")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("requires a value", proc.stderr)

    def test_timeout_range_is_usage_error(self) -> None:
        proc = cli("status", "--timeout", "0")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("1..60000", proc.stderr)

    def test_recorder_rejects_trigger_only_flags(self) -> None:
        proc = cli("arm", "--priority", "3")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("only apply to trigger", proc.stderr)

    def test_verify_requires_package(self) -> None:
        proc = cli("verify")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("needs a package directory", proc.stderr)

    def test_key_only_for_verify(self) -> None:
        proc = cli("status", "--key", "k")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("--key only applies to verify", proc.stderr)


class ControlTests(unittest.TestCase):
    def _path(self, tmp: str, name: str = "ctl.sock") -> str:
        return os.path.join(tmp, name)

    def test_status_json_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = self._path(tmp)
            with FakeControlServer(path, STATUS_JSON) as server:
                proc = cli("status", "--socket", path, "--json")
            self.assertEqual(server.received, "status-json")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            doc = json.loads(proc.stdout)
            self.assertEqual(doc["kind"], "thyme-recorder-status")
            self.assertEqual(doc["schema"], 1)
            self.assertFalse(doc["armed"])
            self.assertEqual(doc["phase"], "post")
            self.assertEqual(doc["pending_bytes"], 4096)
            self.assertTrue(doc["disk_full"])

    def test_status_text_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = self._path(tmp)
            with FakeControlServer(path, STATUS_TEXT) as server:
                proc = cli("status", "--socket", path)
            self.assertEqual(server.received, "status")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stdout.strip(), STATUS_TEXT)

    def test_control_socket_alias(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = self._path(tmp)
            with FakeControlServer(path) as server:
                proc = cli("disarm", "--control-socket", path, "--json")
            self.assertEqual(server.received, "disarm")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            doc = json.loads(proc.stdout)
            self.assertEqual(doc["kind"], "thyme-recorder-control")
            self.assertEqual(doc["command"], "disarm")
            self.assertTrue(doc["ok"])

    def test_replay_rejection_is_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = self._path(tmp)
            with FakeControlServer(path, code=2):
                proc = cli("arm", "--socket", path, "--json")
            self.assertEqual(proc.returncode, 1)
            self.assertIn("replay_rejected", proc.stderr)
            self.assertIn("new sequence", proc.stderr)
            doc = json.loads(proc.stdout)
            self.assertFalse(doc["ok"])
            self.assertEqual(doc["code"], "replay_rejected")

    def test_status_timeout_exits_five(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = self._path(tmp, "silent.sock")
            with FakeControlServer(path, None, respond=False):
                proc = cli("status", "--socket", path, "--timeout", "150",
                           "--json")
            self.assertEqual(proc.returncode, 5)
            self.assertIn("timed out", proc.stderr)
            doc = json.loads(proc.stdout)
            self.assertFalse(doc["ok"])
            self.assertEqual(doc["kind"], "thyme-recorder-status")

    def test_unreachable_socket_exits_five(self) -> None:
        proc = cli("arm", "--socket", "/tmp/thyme-does-not-exist.sock",
                   "--json")
        self.assertEqual(proc.returncode, 5)
        self.assertIn("unreachable", proc.stderr)
        doc = json.loads(proc.stdout)
        self.assertFalse(doc["ok"])
        self.assertEqual(doc["kind"], "thyme-recorder-control")


class DropFirstTriggerServer:
    REQUEST = struct.Struct("@IHHiQ64sQI4xQI4x")
    RESPONSE = struct.Struct("@IHHQiIIIQI4x")

    def __init__(self, path: str) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(path)
        self.sock.settimeout(5.0)
        self.requests = []
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        try:
            first, _ = self.sock.recvfrom(4096)
            second, addr = self.sock.recvfrom(4096)
            self.requests = [first, second]
            request_id = self.REQUEST.unpack(second)[8]
            values = (0x47495254, 4, 0, 7, 10, 4, 0, 0, request_id, 0)
            crc = zlib.crc32(self.RESPONSE.pack(*values)[:40])
            reply = self.RESPONSE.pack(*values[:-1], crc)
            self.sock.sendto(reply, addr)
        finally:
            self.sock.close()

    def __enter__(self) -> "DropFirstTriggerServer":
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.thread.join(timeout=6.0)


class TriggerTests(unittest.TestCase):
    def test_unreachable_trigger_socket_json(self) -> None:
        proc = cli("manual", "--socket", "/tmp/thyme-trigger-does-not-exist.sock",
                   "--json")
        self.assertEqual(proc.returncode, 5)
        doc = json.loads(proc.stdout)
        self.assertEqual(doc["kind"], "thyme-trigger-result")
        self.assertFalse(doc["ok"])
        self.assertFalse(doc["transport_ok"])
        self.assertFalse(doc["result_unknown"])
        self.assertEqual(doc["source"], "manual")

    def test_v4_retry_reuses_request_id(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "trigger.sock")
            with DropFirstTriggerServer(path) as server:
                proc = cli("manual", "--socket", path, "--timeout", "150",
                           "--json")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(len(server.requests), 2)
            self.assertEqual(server.requests[0], server.requests[1])
            fields = DropFirstTriggerServer.REQUEST.unpack(server.requests[1])
            self.assertEqual(fields[1], 4)
            self.assertNotEqual(fields[8], 0)
            self.assertEqual(zlib.crc32(server.requests[1][:112]), fields[9])
            doc = json.loads(proc.stdout)
            self.assertEqual(doc["request_id"], fields[8])
            self.assertEqual(doc["attempts"], 2)
            self.assertEqual(doc["retries"], 1)
            self.assertTrue(doc["deduplicated"])
            self.assertFalse(doc["result_unknown"])


class VerifyTests(unittest.TestCase):
    def test_missing_package_fails_with_json_shape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package = os.path.join(tmp, "no-such-package")
            proc = cli("verify", package, "--json")
            self.assertEqual(proc.returncode, 1)
            doc = json.loads(proc.stdout)
            self.assertEqual(doc["kind"], "thyme-recorder-verify")
            self.assertEqual(doc["schema"], 1)
            self.assertFalse(doc["ok"])
            self.assertFalse(doc["sealed"])
            self.assertIn(doc["manifest"], {"absent", "invalid"})
            self.assertIn("files_ok", doc)
            self.assertIn("error", doc)


if __name__ == "__main__":
    if BINARY is None or not BINARY.is_file():
        print("SKIP: triggerctl binary not provided", file=sys.stderr)
        sys.exit(77)
    unittest.main()
