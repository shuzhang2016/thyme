"""Tests for the pure-Python thyme reader (ECO-03).

The C++ fixture (``tests/thyme_py_fixture.cpp``) writes the packages so this
test never hand-assembles the format: header, registry, manifest, footer index
and record CRCs all come from the production writer.

Run standalone with ``THYME_PY_FIXTURE=/path/to/thyme_py_fixture`` or via
``tests/run_tests.py <fixture>``.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

PY_ROOT = Path(__file__).resolve().parents[1]
if str(PY_ROOT) not in sys.path:
    sys.path.insert(0, str(PY_ROOT))

import thyme_py  # noqa: E402
from thyme_py import (  # noqa: E402
    Record,
    ThymeBinFile,
    ThymeCrcError,
    ThymeFormatError,
    ThymePackage,
    decode_record,
    load_schema,
)

BASE = 1_000_000_000
MANIFEST_HASH = 0x9914c81f74037d59


class ThymePyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        fixture = os.environ.get("THYME_PY_FIXTURE")
        if not fixture or not os.path.exists(fixture):
            raise unittest.SkipTest(
                "THYME_PY_FIXTURE is not set to the C++ fixture executable")
        cls.tmp = Path(tempfile.mkdtemp(prefix="thyme_py_test."))
        subprocess.run([fixture, str(cls.tmp)], check=True)
        cls.pkg_dir = cls.tmp / "pkg-v4"
        cls.v4_path = cls.pkg_dir / "small.tbin"
        cls.v3_path = cls.tmp / "legacy-v3.tbin"
        cls.v2_path = cls.tmp / "legacy-v2.tbin"
        cls.v1_path = cls.tmp / "stream-v1.tbin"

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    # -- binary parser -----------------------------------------------------
    def test_v4_header_keyed_registry_manifest(self) -> None:
        with ThymeBinFile(self.v4_path) as binary:
            self.assertEqual(binary.header.file_abi_version, 4)
            self.assertEqual(binary.header.device_id, 0)
            self.assertEqual(binary.header.session_id, 0x1234)
            self.assertEqual(binary.header.start_mono_ns, BASE)
            self.assertEqual(binary.header.struct_count, 2)
            self.assertEqual([d.struct_id for d in binary.registry], [0, 9])
            pod, cam = binary.registry
            self.assertEqual(pod.name, "Pod16")
            self.assertEqual(pod.contract_version, 1)
            self.assertEqual(pod.payload_size, 16)
            self.assertEqual(pod.channel, "cam.pod16")
            self.assertEqual(pod.record_key, "cam.pod16")
            self.assertEqual(pod.key, "cam.pod16")
            self.assertNotEqual(pod.layout_hash, 0)
            self.assertEqual(cam.name, "CamIdx")
            self.assertEqual(cam.channel, "cam.idx")
            # Stored record_key may be empty; the channel is the fallback key.
            self.assertEqual(cam.record_key, "")
            self.assertEqual(cam.key, "cam.idx")
            self.assertEqual(cam.layout_hash, 0)
            self.assertEqual(binary.manifest_status, "ok")
            self.assertEqual(binary.manifest.manifest_version, 1)
            self.assertEqual(binary.manifest.manifest_hash, MANIFEST_HASH)
            # header + two 128-byte ABI-11 registry entries + manifest block
            self.assertEqual(binary.data_offset, 40 + 2 * 128 + 32)
            self.assertEqual(binary.struct(0).channel, "cam.pod16")
            self.assertIsNone(binary.struct(77))

    def test_v3_legacy_registry_with_manifest(self) -> None:
        with ThymeBinFile(self.v3_path) as binary:
            self.assertEqual(binary.header.file_abi_version, 3)
            self.assertEqual(binary.header.session_id, 0x1234)
            self.assertEqual(binary.manifest_status, "ok")
            self.assertEqual(binary.manifest.manifest_hash, MANIFEST_HASH)
            # v1-v3 registry entries are 40 bytes and carry no ABI-11 keys.
            self.assertEqual(binary.data_offset, 40 + 2 * 40 + 32)
            # A v3 stream capture has no footer index.
            self.assertEqual(binary.index_status, "absent")
            self.assertEqual(binary.records_end, binary.file_size)
            self.assertEqual([d.struct_id for d in binary.registry], [0, 9])
            for desc in binary.registry:
                self.assertEqual(desc.channel, "")
                self.assertEqual(desc.record_key, "")
                self.assertEqual(desc.layout_hash, 0)
                self.assertIsNone(desc.key)
            records = binary.records()
        self.assertEqual(len(records), 8)
        self.assertEqual(records[0].payload, struct.pack("<QII", 0, 0, 0))

    def test_footer_index(self) -> None:
        with ThymeBinFile(self.v4_path) as binary:
            self.assertEqual(binary.index_status, "ok")
            self.assertIsNone(binary.index_error)
            footer = binary.index_footer
            self.assertEqual(footer.index_version, 1)
            self.assertEqual(footer.block_size, thyme_py.INDEX_BLOCK_SIZE)
            self.assertEqual(footer.block_count, 1)
            self.assertEqual(footer.total_records, 7)
            self.assertEqual(footer.first_mono_ns, BASE + 100)
            self.assertEqual(footer.last_mono_ns, BASE + 600)
            self.assertEqual(binary.records_end, footer.entries_offset)
            self.assertEqual(binary.index_entries[0].offset,
                             binary.data_offset)
            self.assertEqual(binary.index_entries[0].mono_ns, BASE + 100)

    def test_v4_records_and_crc(self) -> None:
        with ThymeBinFile(self.v4_path) as binary:
            records = binary.records()
        self.assertEqual([r.kind for r in records],
                         ["data"] * 5 + ["gap", "trigger", "session"])
        data = records[:5]
        for i, rec in enumerate(data):
            self.assertEqual(rec.struct_id, 0)
            self.assertEqual(rec.record_sequence, i)
            self.assertEqual(rec.source_sequence, i)
            self.assertEqual(rec.mono_ns, BASE + 100 * (i + 1))
            self.assertEqual(len(rec.payload), 16)
            a, b, c = struct.unpack("<QII", rec.payload)
            self.assertEqual((a, b, c), (i, i * 2, i * 3))
            self.assertIs(rec.crc_ok, True)
            self.assertEqual(records[i + 1].offset, rec.next_offset)

        gap = records[5]
        self.assertEqual(gap.struct_id, 0)
        self.assertEqual(gap.mono_ns, BASE + 550)
        self.assertEqual(gap.reason, 3)
        self.assertEqual(gap.missed_count, 5)
        self.assertIs(gap.crc_ok, True)

        trigger = records[6]
        self.assertEqual(trigger.trigger_id, 42)
        self.assertEqual(trigger.record_sequence, 42)
        self.assertEqual(trigger.mono_ns, BASE + 600)
        self.assertEqual(trigger.pre_roll_ns, 1000)
        self.assertEqual(trigger.post_roll_ns, 2000)
        self.assertIs(trigger.crc_ok, True)

        session = records[7]
        self.assertEqual(session.session_id, 0x5678)
        self.assertEqual(session.reason, 2)
        self.assertIsNone(session.mono_ns)
        self.assertIs(session.crc_ok, True)

    def test_crc_disabled_does_not_claim_verification(self) -> None:
        with ThymeBinFile(self.v4_path, verify_crc=False) as binary:
            records = binary.records()
        self.assertTrue(all(r.crc_ok is None for r in records))

    def test_corrupt_payload_raises_crc_error(self) -> None:
        bad = self.tmp / "corrupt.tbin"
        shutil.copyfile(self.v4_path, bad)
        with ThymeBinFile(bad) as binary:
            first = next(binary.iter_records())
            payload_at = first.offset + 32
        with open(bad, "r+b") as fp:
            fp.seek(payload_at)
            byte = fp.read(1)
            fp.seek(payload_at)
            fp.write(bytes([byte[0] ^ 0xFF]))
        with ThymeBinFile(bad) as binary:
            with self.assertRaises(ThymeCrcError) as ctx:
                binary.records()
        self.assertEqual(ctx.exception.offset, first.offset)

    def test_legacy_v2_no_manifest(self) -> None:
        with ThymeBinFile(self.v2_path) as binary:
            self.assertEqual(binary.header.file_abi_version, 2)
            self.assertEqual(binary.manifest_status, "absent")
            self.assertIsNone(binary.manifest)
            self.assertEqual(binary.index_status, "ok")
            records = binary.records()
        self.assertEqual(len(records), 2)
        self.assertEqual([r.mono_ns for r in records], [BASE + 10, BASE + 20])

    def test_stream_v1_without_footer(self) -> None:
        with ThymeBinFile(self.v1_path) as binary:
            self.assertEqual(binary.header.file_abi_version, 1)
            self.assertEqual(binary.index_status, "absent")
            self.assertEqual(binary.records_end, binary.file_size)
            records = binary.records()
        self.assertEqual(len(records), 2)
        self.assertEqual([r.mono_ns for r in records], [BASE + 30, BASE + 40])

    def test_bad_magic_rejected(self) -> None:
        bad = self.tmp / "bad-magic.tbin"
        bad.write_bytes(b"XXXX" + bytes(36))
        with self.assertRaises(ThymeFormatError):
            ThymeBinFile(bad)

    def test_future_version_rejected(self) -> None:
        bad = self.tmp / "future-version.tbin"
        shutil.copyfile(self.v4_path, bad)
        with open(bad, "r+b") as fp:
            fp.seek(4)
            fp.write(struct.pack("<H", 5))
        with self.assertRaises(ThymeFormatError):
            ThymeBinFile(bad)

    def test_iter_from_uses_index(self) -> None:
        with ThymeBinFile(self.v4_path) as binary:
            records = list(binary.iter_from(BASE + 250))
        self.assertEqual(records[0].mono_ns, BASE + 300)
        self.assertEqual(records[-1].kind, "session")

    # -- schema sidecar ----------------------------------------------------
    def test_schema_sidecar(self) -> None:
        schema = load_schema(self.pkg_dir)
        self.assertIsNotNone(schema)
        assert schema is not None
        self.assertEqual(schema.schema_version, 3)
        self.assertFalse(schema.partial)
        self.assertEqual(schema.manifest_hash, MANIFEST_HASH)
        self.assertEqual(schema.manifest_version, 1)
        self.assertEqual(schema.frame_header_bytes, 40)
        by_id = schema.by_id()
        pod = by_id[0]
        self.assertEqual(pod.name, "Pod16")
        self.assertEqual(pod.layout, "known")
        self.assertEqual(pod.payload_offset, 0)
        self.assertEqual(pod.contract_bytes, 16)
        self.assertEqual([f.name for f in pod.fields], ["a", "b", "c"])
        self.assertEqual(by_id[9].layout, "unavailable")
        # ABI-11 identity, both views of it.
        self.assertEqual(pod.channel, "cam.pod16")
        self.assertEqual(pod.record_key, "cam.pod16")
        self.assertNotEqual(pod.layout_hash, 0)
        self.assertEqual(by_id[9].channel, "cam.idx")
        self.assertEqual(by_id[9].record_key, "cam.idx")
        self.assertEqual(by_id[9].layout_hash, 0)
        self.assertEqual([c.struct_id for c in schema.channels], [0, 9])
        self.assertEqual(schema.channels[0].channel, "cam.pod16")
        self.assertEqual(schema.channels[0].layout_hash, pod.layout_hash)
        self.assertEqual(schema.channels[1].record_key, "cam.idx")
        self.assertEqual(set(schema.by_key()), {"cam.pod16", "cam.idx"})
        self.assertIs(schema.by_key()["cam.pod16"], pod)
        self.assertEqual(schema.payload_profiles, ())

    def test_schema_v1_sidecar_remains_readable(self) -> None:
        legacy = self.tmp / "legacy-schema"
        legacy.mkdir(exist_ok=True)
        body = {
            "format": "thyme-record-schema",
            "schema_version": 1,
            "partial": False,
            "record_manifest": {"hash": None, "version": None,
                                "channels": None},
            "frame_header": {"bytes": 36, "fields": []},
            "records": [{
                "struct_id": 0,
                "name": "Pod16",
                "contract_version": 1,
                "payload_size": 16,
                "frame_header_bytes": 36,
                "layout": "unavailable",
                "payload_offset": None,
                "contract_bytes": None,
                "fields": None,
            }],
        }
        (legacy / "schema.json").write_text(json.dumps(body), encoding="utf-8")
        schema = load_schema(legacy)
        self.assertIsNotNone(schema)
        assert schema is not None
        self.assertEqual(schema.schema_version, 1)
        self.assertEqual(schema.channels, ())
        self.assertEqual(schema.records[0].channel, "")
        self.assertEqual(schema.records[0].layout_hash, 0)
        self.assertIsNone(schema.records[0].key)
        self.assertEqual(schema.by_key(), {})

    def test_schema_v2_requires_identity(self) -> None:
        manifest = json.loads(
            (self.pkg_dir / "schema.json").read_text(encoding="utf-8"))
        manifest["records"][0].pop("channel")
        broken = self.tmp / "broken-schema"
        broken.mkdir(exist_ok=True)
        (broken / "schema.json").write_text(json.dumps(manifest),
                                           encoding="utf-8")
        with self.assertRaises(thyme_py.ThymeSchemaError):
            load_schema(broken)

    def test_decode_record(self) -> None:
        schema = load_schema(self.pkg_dir)
        with ThymeBinFile(self.v4_path) as binary:
            record = next(binary.iter_records())
        self.assertEqual(decode_record(schema, record),
                         {"a": 0, "b": 0, "c": 0})
        with ThymeBinFile(self.v4_path) as binary:
            records = binary.records()
        self.assertEqual(decode_record(schema, records[4]),
                         {"a": 4, "b": 8, "c": 12})
        with self.assertRaises(thyme_py.ThymeSchemaError):
            decode_record(schema, Record(kind="gap", offset=0, next_offset=0))

    def test_schema_absent_returns_none(self) -> None:
        self.assertIsNone(load_schema(self.tmp))

    # -- package facade ----------------------------------------------------
    def test_package_records_and_info(self) -> None:
        pkg = ThymePackage(self.pkg_dir)
        self.assertEqual(pkg.kind, "package")
        self.assertEqual(pkg.files, ["small.tbin"])
        self.assertEqual(pkg.schema_state, "ok")
        items = pkg.records()
        self.assertEqual(len(items), 8)
        self.assertTrue(all(item.file == "small.tbin" for item in items))
        info = pkg.info()
        self.assertEqual(info["kind"], "package")
        self.assertEqual(info["schema"]["state"], "ok")
        self.assertEqual(info["files"][0]["counts"]["data"], 5)
        self.assertEqual(info["files"][0]["counts"]["session"], 1)
        self.assertEqual(info["files"][0]["file_abi_version"], 4)
        self.assertEqual(info["files"][0]["structs"][0]["channel"],
                         "cam.pod16")
        self.assertEqual(info["schema"]["channels"], 2)

    def test_package_single_file(self) -> None:
        pkg = ThymePackage(self.v4_path)
        self.assertEqual(pkg.kind, "file")
        self.assertEqual(pkg.files, ["small.tbin"])
        self.assertEqual(pkg.schema_state, "ok")

    def test_package_missing_path(self) -> None:
        with self.assertRaises(thyme_py.ThymeError):
            ThymePackage(self.tmp / "does-not-exist")

    # -- CLI ---------------------------------------------------------------
    def _cli(self, *args: str) -> subprocess.CompletedProcess:
        env = dict(os.environ)
        env["PYTHONPATH"] = str(PY_ROOT)
        return subprocess.run(
            [sys.executable, "-m", "thyme_py", *args],
            capture_output=True, text=True, env=env, check=False)

    def test_cli_info_json(self) -> None:
        proc = self._cli("info", str(self.pkg_dir), "--json")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        info = json.loads(proc.stdout)
        self.assertEqual(info["format"], "thyme-python-info")
        self.assertEqual(info["schema"]["manifest"]["hash"],
                         f"0x{MANIFEST_HASH:016x}")
        self.assertEqual(info["files"][0]["counts"]["trigger"], 1)

    def test_cli_dump_limit_and_hex(self) -> None:
        proc = self._cli("dump", str(self.pkg_dir), "--limit", "3", "--hex")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        lines = [line for line in proc.stdout.splitlines() if line]
        self.assertEqual(len(lines), 3)
        first = json.loads(lines[0])
        self.assertEqual(first["kind"], "data")
        self.assertEqual(first["file"], "small.tbin")
        self.assertEqual(first["payload_hex"], "00" * 16)

    def test_cli_missing_path(self) -> None:
        proc = self._cli("info", str(self.tmp / "nope"))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("thyme_py: error:", proc.stderr)


if __name__ == "__main__":
    unittest.main()
