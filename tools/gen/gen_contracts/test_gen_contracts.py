#!/usr/bin/env python3
"""Unit tests for gen_contracts.py (template reorg phase 02)."""

from __future__ import annotations

import copy
import json
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
GEN = TOOLS_DIR / "gen_contracts.py"
TESTDATA = TOOLS_DIR / "testdata"

# Import the module directly so the layout model can be unit-tested against
# hand-computed numbers (the emit path is covered by the CLI golden tests).
sys.path.insert(0, str(TOOLS_DIR))
import gen_contracts  # noqa: E402


def run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(GEN), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def _minimal_doc() -> dict:
    return {
        "version": 1,
        "namespace": "aeb",
        "frame_magic": "TSTH",
        "frame_version": 7,
        "types": [
            {"name": "A_t", "fields": [{"name": "x", "type": "uint8"}]},
            {"name": "B_t", "fields": [{"name": "a", "type": "A_t"}]},
        ],
    }


def _length_of_doc() -> dict:
    return {
        "version": 1,
        "namespace": "aeb",
        "frame_magic": "TSTH",
        "frame_version": 7,
        "types": [
            {"name": "Target_t", "fields": [{"name": "id", "type": "uint32"}]},
            {"name": "Packet_t", "fields": [
                {"name": "n", "type": "uint32"},
                {"name": "targets", "type": "Target_t", "count": 4, "length_of": "n"},
            ]},
        ],
    }


class GenContractsTests(unittest.TestCase):
    def _run_doc(self, doc: dict) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp:
            infile = Path(tmp) / "d.json"
            infile.write_text(json.dumps(doc), encoding="utf-8")
            return run_cli("--in", str(infile), "--out", str(Path(tmp) / "out"), "--check")

    def test_golden_emit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp,
                           "--force", "--date", "2026-09-20")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            golden = (TESTDATA / "sample_types.h").read_bytes()
            emitted = (Path(tmp) / "include" / "aeb" / "contracts" / "types.h").read_bytes()
            self.assertEqual(emitted, golden)
            inc = (Path(tmp) / "template" / "contracts_include.txt").read_text()
            self.assertEqual(inc, "aeb/contracts/types.h\n")

    def test_golden_emit_contract_sizes(self) -> None:
        doc = json.loads((TESTDATA / "sample.json").read_text(encoding="utf-8"))
        layouts = gen_contracts.compute_layout(doc)
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp,
                           "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            body = (Path(tmp) / "template" / "contract_sizes.txt").read_text()
            rows = {}
            for line in body.splitlines():
                if not line or line.startswith("#"):
                    continue
                name, size = line.split("|")
                rows[name] = int(size)
            expected = {t["name"]: layouts[t["name"]]["size"]
                        for t in doc["types"]}
            self.assertEqual(rows, expected)

    def test_golden_emit_contract_layout(self) -> None:
        # ABI-09: runtime field-layout table for the package schema sidecar.
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp,
                           "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            golden = (TESTDATA / "sample_contract_layout.h").read_bytes()
            emitted = (Path(tmp) / "include" / "aeb" / "contracts" /
                       "contract_layout.h").read_bytes()
            self.assertEqual(emitted, golden)
            marker = (Path(tmp) / "template" / "contracts_layout.txt").read_text()
            self.assertEqual(marker, "aeb/contracts/contract_layout.h\n")

    def test_contract_layout_model(self) -> None:
        # The table comes from compute_layout() and the struct table carries
        # the alignment the recorder needs to locate the payload in Frame<T>.
        doc = json.loads((TESTDATA / "sample.json").read_text(encoding="utf-8"))
        layouts = gen_contracts.compute_layout(doc)
        body = gen_contracts.emit_contract_layout(doc)
        for name, info in layouts.items():
            self.assertIn(f'"{name}", {info["size"]}u, '
                          f'{len(info["fields"])}u, {info["align"]}u, '
                          f'k{name}Fields', body)
            self.assertIn(
                f"static_assert(kContracts["
                f"{[t['name'] for t in doc['types']].index(name)}]"
                f".align == alignof({name})", body)
        # Array stride/count survive into the runtime table.
        self.assertIn('{"wheel_speed", "float32", 8u, 16u, 4u, 4u}', body)
        self.assertIn("FindContractLayout", body)

    def test_golden_emit_contracts_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            golden = (TESTDATA / "sample_contracts_json.h").read_bytes()
            emitted = (Path(tmp) / "include" / "aeb" / "contracts" / "contracts_json.h").read_bytes()
            self.assertEqual(emitted, golden)

    def test_golden_emit_thyme_types(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp,
                           "--force", "--date", "2026-09-20")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            golden = (TESTDATA / "sample_thyme_types.h").read_bytes()
            emitted = (Path(tmp) / "include" / "aeb" / "contracts" / "thyme_types.h").read_bytes()
            self.assertEqual(emitted, golden)

    def test_contracts_json_structured_serializer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            body = (Path(tmp) / "include" / "aeb" / "contracts" / "contracts_json.h").read_text()
            self.assertIn("namespace thyme_contracts_json", body)
            self.assertIn("inline void ToJson(const thyme::Frame<EgoMotion_t>& v, std::string& out)", body)
            self.assertIn("inline const char* kSchema_EgoMotion_t = ", body)
            self.assertIn('\\"wheel_speed\\":{\\"type\\":\\"array\\"', body)
            # nested struct serialized via the bare-payload overload, primitives
            # via Append* reading the payload member
            self.assertIn("ToJson(v.payload.ego, out);", body)
            self.assertIn("AppendF32(out, v.payload.vx_mps);", body)
            # no embedded header key or header schema survives (F2)
            self.assertNotIn("kSchema_FrameHeader", body)
            self.assertNotIn("ToJson(v.hdr", body)
            self.assertNotIn('\\"hdr\\"', body)

    def test_contracts_json_opaque_envelope(self) -> None:
        doc = {
            "version": 1,
            "namespace": "aeb",
            "frame_magic": "TSTH",
            "frame_version": 7,
            "types": [
                {"name": "ImageBuffer_t",
                 "fields": [{"name": "payload", "type": "uint8", "count": 64}]},
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            infile = Path(tmp) / "d.json"
            infile.write_text(json.dumps(doc), encoding="utf-8")
            proc = run_cli("--in", str(infile), "--out", str(Path(tmp) / "out"), "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            body = (Path(tmp) / "out" / "include" / "aeb" / "contracts" / "contracts_json.h").read_text()
            self.assertIn('out += "{\\"mono_ns\\":";', body)
            self.assertIn("AppendUInt(out, v.hdr.mono_ns);", body)
            self.assertIn("B64(out, reinterpret_cast<const uint8_t*>(&v.payload), sizeof(v.payload));", body)
            self.assertIn('\\"payload_b64\\":{\\"type\\":\\"string\\"}', body)
            # opaque schema must NOT expose hdr/payload fields (envelope only)
            self.assertNotIn('\\"hdr\\"', body.split("kSchema_ImageBuffer_t")[1])


    def test_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run_cli("--in", str(TESTDATA / "sample.json"), "--out", str(Path(tmp) / "a"), "--force")
            run_cli("--in", str(TESTDATA / "sample.json"), "--out", str(Path(tmp) / "b"), "--force")
            a = (Path(tmp) / "a" / "include" / "aeb" / "contracts" / "types.h").read_bytes()
            b = (Path(tmp) / "b" / "include" / "aeb" / "contracts" / "types.h").read_bytes()
            self.assertEqual(a, b)

    def test_check_writes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--check")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("include/aeb/contracts/types.h", proc.stdout)
            self.assertIn("template/contracts_include.txt", proc.stdout)
            self.assertFalse((Path(tmp) / "include").exists())
            self.assertFalse((Path(tmp) / "template").exists())

    def test_no_force_refuses_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            header = Path(tmp) / "include" / "aeb" / "contracts" / "types.h"
            before = header.read_bytes()
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp)
            self.assertEqual(proc.returncode, 2)
            self.assertIn("refusing to overwrite", proc.stderr)
            self.assertEqual(header.read_bytes(), before)

    def test_with_fill_stub(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli(
                "--in", str(TESTDATA / "sample.json"), "--out", tmp,
                "--force", "--with-fill-stub",
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            header = (Path(tmp) / "include" / "aeb" / "contracts" / "types.h").read_text()
            self.assertIn("#include <cstring>", header)
            self.assertIn("void FillStub(", header)
            self.assertIn("thyme::FrameInit(msg, seq, mono_ns, session);", header)
            self.assertIn("thyme::FrameSeal(msg, kFrameMagic, kFrameVersion);", header)
            self.assertNotIn("FrameHeader hdr;", header)

    def test_length_of_emits_debug_only_seal_check(self) -> None:
        header = gen_contracts.emit_header(_length_of_doc(), with_fill_stub=True)
        self.assertIn("#ifndef NDEBUG", header)
        self.assertIn(
            "inline void FrameSeal(Frame<Packet_t>& msg, uint32_t magic, uint16_t version)",
            header,
        )
        self.assertIn("FrameSeal<Frame<Packet_t>>(msg, magic, version);", header)
        self.assertIn("if (msg.payload.n > 4) {", header)
        self.assertNotIn("msg.payload.n < 0", header)
        self.assertIn("msg.hdr.valid = 0;", header)
        self.assertIn("#endif  // NDEBUG", header)
        self.assertLess(header.index("FrameSeal(Frame<Packet_t>&"), header.index("void FillStub("))

        signed_doc = _length_of_doc()
        signed_doc["types"][1]["fields"][0]["type"] = "int32"
        signed_header = gen_contracts.emit_header(signed_doc, with_fill_stub=True)
        self.assertIn("if (msg.payload.n < 0 || msg.payload.n > 4) {", signed_header)

    def test_duplicate_type_name(self) -> None:
        doc = _minimal_doc()
        doc["types"].append({"name": "A_t", "fields": [{"name": "y", "type": "uint8"}]})
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("duplicate type name", proc.stderr)

    def test_forward_type_ref(self) -> None:
        doc = _minimal_doc()
        doc["types"] = [
            {"name": "B_t", "fields": [{"name": "a", "type": "A_t"}]},
            {"name": "A_t", "fields": [{"name": "x", "type": "uint8"}]},
        ]
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("not a primitive or a previously-defined type", proc.stderr)

    def test_bad_primitive(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["fields"][0]["type"] = "string"
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("not a primitive or a previously-defined type", proc.stderr)

    def test_bad_count(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["fields"][0]["count"] = 0
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("count must be an integer >= 1", proc.stderr)

    def test_unknown_key(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["fields"][0]["bogus"] = 1
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("unknown key", proc.stderr)

    def test_graph_coverage_missing_type(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            graph = Path(tmp) / "g.yml"
            graph.write_text(
                "version: 1\n"
                "edges:\n"
                "  - from: a\n"
                "    to: b\n"
                "    channel: c\n"
                "    cpp_type: EgoMotion_t\n"
                "  - from: a\n"
                "    to: b\n"
                "    channel: c2\n"
                "    cpp_type: Missing_t\n",
                encoding="utf-8",
            )
            proc = run_cli(
                "--in", str(TESTDATA / "sample.json"),
                "--out", str(Path(tmp) / "out"), "--check", "--graph", str(graph),
            )
            self.assertEqual(proc.returncode, 2)
            self.assertIn("Missing_t", proc.stderr)
            self.assertIn("missing from datatypes.json", proc.stderr)

    def test_bool_field_warns(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["fields"][0]["type"] = "bool"
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 0)
        self.assertIn("bool", proc.stderr)

    def test_missing_frame_magic(self) -> None:
        doc = _minimal_doc()
        del doc["frame_magic"]
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("frame_magic", proc.stderr)

    def test_bad_frame_magic(self) -> None:
        doc = _minimal_doc()
        doc["frame_magic"] = "ABC"
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("frame_magic", proc.stderr)

    def test_bad_frame_version(self) -> None:
        doc = _minimal_doc()
        doc["frame_version"] = 0
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("frame_version", proc.stderr)

    def test_layout_checks_golden(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            golden = (TESTDATA / "sample_layout_checks.h").read_bytes()
            emitted = (Path(tmp) / "include" / "aeb" / "contracts" / "layout_checks.h").read_bytes()
            self.assertEqual(emitted, golden)

    def test_layout_checks_content(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            body = (Path(tmp) / "include" / "aeb" / "contracts" / "layout_checks.h").read_text()
            self.assertIn("#ifdef THYME_LAYOUT_CHECKS", body)
            self.assertIn("static_assert(sizeof(EgoMotion_t) == sizeof(rte::EgoMotion)", body)
            self.assertIn(
                "static_assert(offsetof(EgoMotion_t, vx_mps) == offsetof(rte::EgoMotion, vx_mps)",
                body,
            )
            self.assertIn("offsetof(EgoMotion_t, wheel_speed)", body)

    def test_layout_checks_absent_without_layout_of(self) -> None:
        doc = _minimal_doc()
        with tempfile.TemporaryDirectory() as tmp:
            infile = Path(tmp) / "d.json"
            infile.write_text(json.dumps(doc), encoding="utf-8")
            proc = run_cli("--in", str(infile), "--out", str(Path(tmp) / "out"), "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertFalse(
                (Path(tmp) / "out" / "include" / "aeb" / "contracts" / "layout_checks.h").exists()
            )

    def test_bad_layout_of(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["layout_of"] = "rte::"
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("layout_of", proc.stderr)

    def test_no_alignas_in_types(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            header = (Path(tmp) / "include" / "aeb" / "contracts" / "types.h").read_text()
            self.assertNotIn("alignas", header)

    def test_emit_frame_constants(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            header = (Path(tmp) / "include" / "aeb" / "contracts" / "types.h").read_text()
            self.assertIn("inline constexpr uint32_t kFrameMagic = 0x54535448u;", header)
            self.assertIn("inline constexpr uint16_t kFrameVersion = 7;", header)
            self.assertIn("using thyme::FrameHeader;", header)
            self.assertIn("using thyme::FramePodWireBytes;", header)
            self.assertIn('#include "thyme/contracts/frame.h"', header)
            # bare payloads: no injected FrameHeader member (plan 01)
            self.assertNotIn("FrameHeader hdr;", header)

    def test_autosar_types(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli(
                "--in", str(TESTDATA / "sample.json"), "--out", tmp,
                "--force", "--with-fill-stub", "--autosar-types",
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            base = Path(tmp) / "include" / "aeb" / "contracts"
            header = (base / "types.h").read_text()
            ttypes = (base / "thyme_types.h").read_text()
            # include the namespace Platform_Types.h, drop the raw <cstdint>
            self.assertIn('#include "aeb/contracts/Platform_Types.h"', header)
            self.assertNotIn("#include <cstdint>", header)
            # AUTOSAR field spellings live in thyme_types.h (sole struct holder)
            self.assertIn("uint8 u8;", ttypes)
            self.assertIn("uint32 u32;", ttypes)
            self.assertIn("sint8 i8;", ttypes)
            self.assertIn("float32 vx_mps;", ttypes)
            self.assertIn("boolean flag;", ttypes)
            # AUTOSAR constants + FillStub signature (still in types.h)
            self.assertIn("inline constexpr uint32 kFrameMagic = 0x54535448u;", header)
            self.assertIn("inline constexpr uint16 kFrameVersion = 7;", header)
            self.assertIn("void FillStub(T& msg, uint32 seq, uint64 mono_ns, uint64 session)", header)
            self.assertNotIn("uint32_t", header)
            self.assertNotIn("uint16_t", header)

    def test_autosar_types_default_off(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            base = Path(tmp) / "include" / "aeb" / "contracts"
            header = (base / "types.h").read_text()
            ttypes = (base / "thyme_types.h").read_text()
            self.assertNotIn("Platform_Types.h", header)
            self.assertIn("uint32_t u32;", ttypes)

    def test_provenance_header(self) -> None:
        doc = _minimal_doc()
        doc["source_version"] = "v1.20"
        with tempfile.TemporaryDirectory() as tmp:
            infile = Path(tmp) / "d.json"
            infile.write_text(json.dumps(doc), encoding="utf-8")
            proc = run_cli("--in", str(infile), "--out", str(Path(tmp) / "out"),
                           "--force", "--date", "2026-09-20")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            header = (Path(tmp) / "out" / "include" / "aeb" / "contracts" / "types.h").read_text()
            lines = header.splitlines()
            self.assertEqual(lines[0], "// Code generated by gen_contracts. DO NOT EDIT.")
            self.assertIn("// Source version: v1.20", lines[:4])
            self.assertIn("// Generated: 2026-09-20", lines[:4])

    def test_provenance_header_defaults_to_today(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            header = (Path(tmp) / "include" / "aeb" / "contracts" / "types.h").read_text()
            self.assertIn("// Generated: ", header)
            # sample.json carries no source_version, so no version line is emitted
            self.assertNotIn("// Source version:", header)

    def test_thyme_types_shareable_header(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli(
                "--in", str(TESTDATA / "sample.json"), "--out", tmp,
                "--force", "--autosar-types", "--date", "2026-09-20",
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            path = Path(tmp) / "include" / "aeb" / "contracts" / "thyme_types.h"
            self.assertTrue(path.exists())
            body = path.read_text()
            # self-contained: includes Platform_Types.h, AUTOSAR structs, provenance
            self.assertIn('#include "Platform_Types.h"', body)
            self.assertIn("struct Primitives_t", body)
            self.assertIn("struct EgoMotion_t", body)
            self.assertIn("float32 vx_mps;", body)
            self.assertIn("uint8 u8;", body)
            self.assertIn("sint8 i8;", body)
            self.assertIn("boolean flag;", body)
            self.assertIn("// Generated: 2026-09-20", body)
            # must be free of runtime glue
            self.assertNotIn("thyme/", body)
            self.assertNotIn("FillStub", body)
            self.assertNotIn("kFrameMagic", body)
            self.assertNotIn("kFrameVersion", body)
            self.assertNotIn("typedef uint8_t uint8;", body)
            self.assertNotIn("FrameHeader", body)

    def test_thyme_types_stdint_default(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "sample.json"), "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            body = (Path(tmp) / "include" / "aeb" / "contracts" / "thyme_types.h").read_text()
            self.assertIn("#include <cstdint>", body)
            self.assertNotIn("Platform_Types.h", body)
            self.assertIn("uint32_t u32;", body)
            self.assertIn("int8_t i8;", body)
            self.assertIn("float vx_mps;", body)
            self.assertIn("bool flag;", body)

    def test_types_h_includes_thyme_types_and_has_no_structs(self) -> None:
        # The struct definitions live solely in thyme_types.h; types.h includes
        # it and must not re-declare them.
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli(
                "--in", str(TESTDATA / "sample.json"), "--out", tmp,
                "--force", "--autosar-types",
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            base = Path(tmp) / "include" / "aeb" / "contracts"
            types_h = (base / "types.h").read_text()
            thyme_types_h = (base / "thyme_types.h").read_text()
            self.assertIn('#include "thyme_types.h"', types_h)
            self.assertNotIn("struct Primitives_t", types_h)
            self.assertNotIn("struct EgoMotion_t", types_h)
            # the structs are present in the shareable header
            self.assertIn("struct Primitives_t", thyme_types_h)
            self.assertIn("struct EgoMotion_t", thyme_types_h)
            # runtime glue still present in types.h
            self.assertIn("using thyme::FrameHeader;", types_h)
            self.assertIn("kFrameMagic", types_h)

    # ---- ABI-03: generated layout asserts ---------------------------------

    def test_golden_emit_layout_sample(self) -> None:
        # Rich fixture: 1-field, padded, array-of-primitive, array-of-struct
        # and nested-struct contracts in one header.
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "layout_sample.json"),
                           "--out", tmp, "--force", "--date", "2026-09-20")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            golden = (TESTDATA / "layout_sample_thyme_types.h").read_bytes()
            emitted = (Path(tmp) / "include" / "lsl" / "contracts" /
                       "thyme_types.h").read_bytes()
            self.assertEqual(emitted, golden)

    def test_layout_model_padding_nested_arrays(self) -> None:
        doc = {
            "types": [
                {"name": "Inner_t", "fields": [
                    {"name": "a", "type": "uint8"},
                    {"name": "b", "type": "uint32"},
                ]},
                {"name": "Outer_t", "fields": [
                    {"name": "head", "type": "uint8"},
                    {"name": "inner", "type": "Inner_t"},
                    {"name": "table", "type": "Inner_t", "count": 3},
                    {"name": "tail", "type": "uint8"},
                ]},
            ],
        }
        layout = gen_contracts.compute_layout(doc)
        inner = layout["Inner_t"]
        self.assertEqual((inner["size"], inner["align"]), (8, 4))
        self.assertEqual([f["offset"] for f in inner["fields"]], [0, 4])
        outer = layout["Outer_t"]
        # head @0, inner @4 (align 4), table @12 (3 * 8, stride == sizeof),
        # tail @36, total rounded to the 4-byte struct alignment.
        self.assertEqual([f["offset"] for f in outer["fields"]], [0, 4, 12, 36])
        self.assertEqual([f["stride"] for f in outer["fields"]], [1, 8, 8, 1])
        self.assertEqual([f["bytes"] for f in outer["fields"]], [1, 8, 24, 1])
        self.assertEqual((outer["size"], outer["align"]), (40, 4))

    def test_layout_model_empty_and_single_field(self) -> None:
        # An empty struct is 1 byte / alignment 1 in C++ (the schema rejects
        # empty contracts today; the model still matches the language rule).
        layout = gen_contracts.compute_layout({
            "types": [
                {"name": "Empty_t", "fields": []},
                {"name": "One_t", "fields": [{"name": "x", "type": "uint16"}]},
            ],
        })
        self.assertEqual((layout["Empty_t"]["size"], layout["Empty_t"]["align"]), (1, 1))
        self.assertEqual(layout["Empty_t"]["fields"], [])
        self.assertEqual((layout["One_t"]["size"], layout["One_t"]["align"]), (2, 2))

    def test_layout_asserts_emitted_for_every_field(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "layout_sample.json"),
                           "--out", tmp, "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            body = (Path(tmp) / "include" / "lsl" / "contracts" /
                    "thyme_types.h").read_text()
            # one-field contract: size + alignment + the only offset
            self.assertIn('static_assert(sizeof(OneField_t) == 8,', body)
            self.assertIn('static_assert(alignof(OneField_t) == 8,', body)
            self.assertIn('static_assert(offsetof(OneField_t, only) == 0,', body)
            # nested struct: offsets include the nested type's own size
            self.assertIn('static_assert(offsetof(Nested_t, padded) == 16,', body)
            self.assertIn('static_assert(alignof(Nested_t) == 8,', body)
            # array: total bytes and element stride (stride includes padding)
            self.assertIn('static_assert(sizeof(Arrays_t::u16x3) == 6,', body)
            self.assertIn('static_assert(sizeof(Arrays_t::u16x3[0]) == 2,', body)
            self.assertIn('static_assert(sizeof(Arrays_t::elems[0]) == 8,', body)
            self.assertIn('static_assert(sizeof(Arrays_t::tail[0]) == 1,', body)
            # generated asserts only: no runtime helpers in the shareable header
            self.assertNotIn("alignas", body)

    def test_layout_asserts_autosar_types(self) -> None:
        # AUTOSAR spellings change the field types only; layout asserts must
        # still be emitted (and <cstddef> must be included for offsetof).
        with tempfile.TemporaryDirectory() as tmp:
            proc = run_cli("--in", str(TESTDATA / "layout_sample.json"),
                           "--out", tmp, "--force", "--autosar-types")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            body = (Path(tmp) / "include" / "lsl" / "contracts" /
                    "thyme_types.h").read_text()
            self.assertIn("#include <cstddef>", body)
            self.assertIn("uint64 only;", body)
            self.assertIn('static_assert(offsetof(OneField_t, only) == 0,', body)
            self.assertIn('static_assert(sizeof(Arrays_t::elems[0]) == 8,', body)

    def test_bytes_must_match_computed_layout(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["bytes"] = 2  # A_t is one uint8 -> natural size 1
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("does not match the computed natural layout size 1", proc.stderr)

    def test_bytes_matching_layout_keeps_wire_size_assert(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["bytes"] = 1
        with tempfile.TemporaryDirectory() as tmp:
            infile = Path(tmp) / "d.json"
            infile.write_text(json.dumps(doc), encoding="utf-8")
            proc = run_cli("--in", str(infile), "--out", str(Path(tmp) / "out"),
                           "--force")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            body = (Path(tmp) / "out" / "include" / "aeb" / "contracts" /
                    "thyme_types.h").read_text()
            self.assertIn('static_assert(sizeof(A_t) == 1, '
                          '"gen_contracts: A_t wire size drift");', body)


class SchemaValidationTests(unittest.TestCase):
    """ABI-08: collect-all schema validation with JSON paths and hard ceilings.

    Every case is a whole-document rule: one violation class per test, always
    asserting the offending JSON path so a contract author can jump straight to
    the field. The fuzz-lite case proves an arbitrary malformed document never
    escapes as a traceback or a partial header tree."""

    def _run_doc(self, doc: dict) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp:
            infile = Path(tmp) / "d.json"
            infile.write_text(json.dumps(doc), encoding="utf-8")
            return run_cli("--in", str(infile), "--out", str(Path(tmp) / "out"),
                           "--check")

    def _assert_rejected(self, doc: dict, *needles: str) -> None:
        self._assert_raw_rejected(json.dumps(doc), *needles)

    def _assert_raw_rejected(self, raw: str, *needles: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            infile = Path(tmp) / "d.json"
            infile.write_text(raw, encoding="utf-8")
            out = Path(tmp) / "out"
            proc = run_cli("--in", str(infile), "--out", str(out), "--force")
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            self.assertNotIn("Traceback", proc.stderr)
            self.assertFalse(out.exists(), "validation failure wrote partial output")
            for needle in needles:
                self.assertIn(needle, proc.stderr)

    def _one_type_doc(self, name: str = "S_t", **type_kwargs) -> dict:
        doc = _minimal_doc()
        doc["types"] = [{"name": name, "fields": [{"name": "x", "type": "uint8"}],
                         **type_kwargs}]
        return doc

    # ---- duplicates / missing structure ----------------------------------

    def test_duplicate_field_name(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["fields"].append({"name": "x", "type": "uint16"})
        self._assert_rejected(
            doc, "types[0].fields[1]: duplicate field name: 'x'")

    def test_empty_fields_rejected(self) -> None:
        doc = self._one_type_doc()
        doc["types"][0]["fields"] = []
        self._assert_rejected(doc, "types[0]: fields must be a non-empty array")

    def test_missing_fields_key_rejected(self) -> None:
        doc = self._one_type_doc()
        del doc["types"][0]["fields"]
        self._assert_rejected(doc, "types[0]: missing required key: fields")

    def test_unknown_top_level_key(self) -> None:
        doc = _minimal_doc()
        doc["bogus"] = 1
        self._assert_rejected(doc, "unknown top-level key: 'bogus'")

    def test_type_entry_not_an_object(self) -> None:
        doc = _minimal_doc()
        doc["types"] = ["A_t"]
        self._assert_rejected(doc, "types[0]: must be an object")

    # ---- C++ identifier validity -----------------------------------------

    def test_type_name_is_cpp_keyword(self) -> None:
        doc = self._one_type_doc(name="class")
        self._assert_rejected(doc, "types[0]: name 'class' is a C++ keyword")

    def test_field_name_is_cpp_keyword(self) -> None:
        doc = self._one_type_doc()
        doc["types"][0]["fields"][0]["name"] = "for"
        self._assert_rejected(doc, "types[0].fields[0]: name 'for' is a C++ keyword")

    def test_type_name_shadows_generated_identifier(self) -> None:
        doc = self._one_type_doc(name="FrameHeader")
        self._assert_rejected(
            doc, "types[0]: name 'FrameHeader' collides with a generated "
                 "contract identifier")

    def test_type_name_shadows_primitive(self) -> None:
        doc = self._one_type_doc(name="uint32")
        self._assert_rejected(
            doc, "types[0]: name 'uint32' collides with a generated "
                 "contract identifier")

    def test_type_name_shadows_stdint_typedef(self) -> None:
        doc = self._one_type_doc(name="uint64_t")
        self._assert_rejected(
            doc, "types[0]: name 'uint64_t' collides with a generated "
                 "contract identifier")

    def test_type_name_reserved_implementation_ident(self) -> None:
        doc = self._one_type_doc(name="_S_t")
        self._assert_rejected(doc, "types[0]: name '_S_t' is reserved for the "
                                   "C++ implementation")
        doc = self._one_type_doc(name="S__t")
        self._assert_rejected(doc, "types[0]: name 'S__t' is reserved for the "
                                   "C++ implementation")

    def test_field_name_reserved_implementation_ident(self) -> None:
        doc = self._one_type_doc()
        doc["types"][0]["fields"][0]["name"] = "__x"
        self._assert_rejected(doc, "types[0].fields[0]: name '__x' is reserved "
                                   "for the C++ implementation")
        doc = self._one_type_doc()
        doc["types"][0]["fields"][0]["name"] = "_X"
        self._assert_rejected(doc, "types[0].fields[0]: name '_X' is reserved "
                                   "for the C++ implementation")

    def test_field_name_equals_struct_name(self) -> None:
        doc = self._one_type_doc(name="S_t")
        doc["types"][0]["fields"][0]["name"] = "S_t"
        self._assert_rejected(
            doc, "types[0].fields[0]: field name 'S_t' collides with its struct name")

    def test_underscored_field_names_stay_legal(self) -> None:
        # A single leading underscore in class scope is legal C++; only '__'
        # and '_' + uppercase are reserved.
        doc = self._one_type_doc(name="S_1_t")
        doc["types"][0]["fields"][0]["name"] = "_x"
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_payload_must_be_the_only_field(self) -> None:
        doc = self._one_type_doc(name="Blob_t")
        doc["types"][0]["fields"] = [
            {"name": "payload", "type": "uint8", "count": 16},
            {"name": "meta", "type": "uint32"},
        ]
        self._assert_rejected(
            doc, "types[0].fields[0]: field 'payload' makes the struct an "
                 "opaque byte envelope; it must be the only field")

    # ---- counts / sizes ---------------------------------------------------

    def test_negative_array_count(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["fields"][0]["count"] = -1
        self._assert_rejected(
            doc, "types[0].fields[0]: count must be an integer >= 1, got -1")

    def test_non_integer_array_count(self) -> None:
        for bad in (1.5, "4"):
            doc = _minimal_doc()
            doc["types"][0]["fields"][0]["count"] = bad
            self._assert_rejected(
                doc, "types[0].fields[0]: count must be an integer >= 1")

    def test_array_count_ceiling(self) -> None:
        doc = _minimal_doc()
        doc["types"][0]["fields"][0]["count"] = gen_contracts.MAX_ARRAY_COUNT + 1
        self._assert_rejected(
            doc,
            f"types[0].fields[0]: count {gen_contracts.MAX_ARRAY_COUNT + 1} "
            f"exceeds the maximum array count {gen_contracts.MAX_ARRAY_COUNT}")

    def test_array_count_at_ceiling_is_legal(self) -> None:
        doc = self._one_type_doc()
        doc["types"][0]["fields"][0]["count"] = gen_contracts.MAX_ARRAY_COUNT
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_struct_size_ceiling(self) -> None:
        # Within the per-field count ceiling, but 8 B × 2^25 = 256 MiB > 64 MiB.
        doc = self._one_type_doc()
        doc["types"][0]["fields"] = [
            {"name": "blob", "type": "uint64", "count": gen_contracts.MAX_ARRAY_COUNT}
        ]
        self._assert_rejected(
            doc,
            "types[0]: natural layout size "
            f"{gen_contracts.MAX_ARRAY_COUNT * 8} exceeds the maximum struct "
            f"size {gen_contracts.MAX_STRUCT_BYTES}")

    def test_total_size_ceiling(self) -> None:
        # Each type is 32 MiB (legal), five of them exceed the 128 MiB total.
        count = gen_contracts.MAX_ARRAY_COUNT
        doc = {
            "version": 1,
            "namespace": "aeb",
            "frame_magic": "TSTH",
            "frame_version": 1,
            "types": [
                {"name": f"Blob{i}_t",
                 "fields": [{"name": "blob", "type": "uint8", "count": count}]}
                for i in range(5)
            ],
        }
        self._assert_rejected(
            doc,
            f"types: total natural layout size {count * 5} exceeds the maximum "
            f"{gen_contracts.MAX_TOTAL_BYTES}")

    def test_bytes_ceiling_and_type(self) -> None:
        doc = self._one_type_doc(bytes=-1)
        self._assert_rejected(doc, "types[0]: bytes must be a positive integer")
        doc = self._one_type_doc(bytes="8")
        self._assert_rejected(doc, "types[0]: bytes must be a positive integer")
        doc = self._one_type_doc(bytes=gen_contracts.MAX_STRUCT_BYTES + 1)
        self._assert_rejected(
            doc,
            f"types[0]: bytes {gen_contracts.MAX_STRUCT_BYTES + 1} exceeds the "
            f"maximum struct size {gen_contracts.MAX_STRUCT_BYTES}")

    def test_too_many_fields(self) -> None:
        doc = self._one_type_doc()
        doc["types"][0]["fields"] = [
            {"name": f"f{i}", "type": "uint8"}
            for i in range(gen_contracts.MAX_FIELDS_PER_STRUCT + 1)
        ]
        self._assert_rejected(
            doc,
            f"types[0]: {gen_contracts.MAX_FIELDS_PER_STRUCT + 1} fields "
            f"exceeds the maximum {gen_contracts.MAX_FIELDS_PER_STRUCT}")

    def test_too_many_types(self) -> None:
        doc = _minimal_doc()
        doc["types"] = [
            {"name": f"T{i}_t", "fields": [{"name": "x", "type": "uint8"}]}
            for i in range(gen_contracts.MAX_TYPES + 1)
        ]
        self._assert_rejected(
            doc,
            f"types: {gen_contracts.MAX_TYPES + 1} entries exceeds the maximum "
            f"{gen_contracts.MAX_TYPES}")

    # ---- nesting depth ----------------------------------------------------

    def _chain(self, depth: int) -> dict:
        types = [{"name": "L0_t", "fields": [{"name": "v", "type": "uint8"}]}]
        for i in range(1, depth):
            types.append({"name": f"L{i}_t",
                          "fields": [{"name": "inner", "type": f"L{i - 1}_t"}]})
        return {"version": 1, "namespace": "aeb", "frame_magic": "TSTH",
                "frame_version": 1, "types": types}

    def test_nesting_depth_at_ceiling_is_legal(self) -> None:
        proc = self._run_doc(self._chain(gen_contracts.MAX_NESTING_DEPTH))
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_nesting_depth_ceiling(self) -> None:
        depth = gen_contracts.MAX_NESTING_DEPTH + 1
        self._assert_rejected(
            self._chain(depth),
            f"types[{depth - 1}].fields[0]: nesting depth {depth} exceeds the "
            f"maximum {gen_contracts.MAX_NESTING_DEPTH}")

    # ---- ABI-14: bounded variable-length arrays ---------------------------

    def test_length_of_accepts_every_scalar_integer_type(self) -> None:
        for integer_type in sorted(gen_contracts.INTEGER_PRIMITIVES):
            with self.subTest(integer_type=integer_type):
                doc = _length_of_doc()
                doc["types"][1]["fields"][0]["type"] = integer_type
                errors, _ = gen_contracts.validate(doc)
                self.assertEqual(errors, [])

    def test_length_of_can_reference_later_field(self) -> None:
        doc = _length_of_doc()
        fields = doc["types"][1]["fields"]
        fields.reverse()
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_length_of_rejects_unknown_and_self_references(self) -> None:
        for length_of, needle in (
            ("missing", "length_of references unknown field 'missing'"),
            ("for", "length_of references unknown field 'for'"),
            ("targets", "length_of must not reference the array field itself"),
        ):
            with self.subTest(length_of=length_of):
                doc = _length_of_doc()
                doc["types"][1]["fields"][1]["length_of"] = length_of
                self._assert_rejected(doc, f"types[1].fields[1]: {needle}")

    def test_length_of_rejects_bad_identifiers(self) -> None:
        for length_of in ("", "9n", "nested.n", "targets[0]", "n::x"):
            with self.subTest(length_of=length_of):
                doc = _length_of_doc()
                doc["types"][1]["fields"][1]["length_of"] = length_of
                self._assert_rejected(
                    doc, "types[1].fields[1]: length_of must be a field identifier")

    def test_length_of_rejects_wrong_types_and_deeply_nested_values(self) -> None:
        deeply_nested = "n"
        for _ in range(12):
            deeply_nested = [deeply_nested]
        for length_of in (None, True, 1.5, [], {"field": "n"}, deeply_nested):
            with self.subTest(length_of=length_of):
                doc = _length_of_doc()
                doc["types"][1]["fields"][1]["length_of"] = length_of
                self._assert_rejected(
                    doc, "types[1].fields[1]: length_of must be a field identifier")

    def test_length_of_rejects_non_integer_array_and_nested_fields(self) -> None:
        for target in (
            {"name": "ratio", "type": "float32"},
            {"name": "flag", "type": "bool"},
            {"name": "one", "type": "Target_t"},
            {"name": "sizes", "type": "uint32", "count": 2},
            {"name": "items", "type": "Target_t", "count": 2},
        ):
            with self.subTest(target=target):
                doc = _length_of_doc()
                doc["types"][1]["fields"].append(target)
                doc["types"][1]["fields"][1]["length_of"] = target["name"]
                self._assert_rejected(
                    doc, "types[1].fields[1]: length_of must reference a scalar integer field")

    def test_length_of_rejects_malformed_referenced_target_type(self) -> None:
        for bad_type in (["uint32"], {"scalar": "uint32"}, [[{"deep": "uint32"}]]):
            with self.subTest(bad_type=bad_type):
                doc = _length_of_doc()
                doc["types"][1]["fields"][0]["type"] = bad_type
                self._assert_rejected(
                    doc,
                    "types[1].fields[0]: bad identifier type",
                    "types[1].fields[1]: length_of must reference a scalar integer field",
                )

    def test_length_of_requires_an_actual_array(self) -> None:
        for count in (None, 1):
            with self.subTest(count=count):
                doc = _length_of_doc()
                array_field = doc["types"][1]["fields"][1]
                if count is None:
                    del array_field["count"]
                else:
                    array_field["count"] = count
                self._assert_rejected(
                    doc, "types[1].fields[1]: length_of requires an array field with count > 1")

    def test_length_of_rejects_bad_array_count_boundaries(self) -> None:
        for count in (False, True, -1, 0, 1.5, "2", [], {},
                      gen_contracts.MAX_ARRAY_COUNT + 1):
            with self.subTest(count=count):
                doc = _length_of_doc()
                doc["types"][1]["fields"][1]["count"] = count
                self._assert_rejected(doc, "types[1].fields[1]")

    def test_length_of_array_count_boundaries_are_legal(self) -> None:
        for count in (2, gen_contracts.MAX_ARRAY_COUNT):
            with self.subTest(count=count):
                doc = _length_of_doc()
                array_field = doc["types"][1]["fields"][1]
                array_field["type"] = "uint8"
                array_field["count"] = count
                proc = self._run_doc(doc)
                self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_length_of_duplicate_json_key_is_rejected(self) -> None:
        for first, second in (
            ("missing", "n"),
            ("n", "missing"),
            ("n", "n"),
            (["n"], "n"),
        ):
            with self.subTest(first=first, second=second):
                replacement = (
                    f'"length_of": {json.dumps(first)}, '
                    f'"length_of": {json.dumps(second)}'
                )
                raw = json.dumps(_length_of_doc()).replace(
                    '"length_of": "n"', replacement)
                self._assert_raw_rejected(
                    raw, "types[1].fields[1]: duplicate key: 'length_of'")

    def test_length_of_shared_by_arrays_is_legal(self) -> None:
        doc = _length_of_doc()
        doc["types"][1]["fields"].append(
            {"name": "aliases", "type": "Target_t", "count": 4, "length_of": "n"})
        proc = self._run_doc(doc)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_length_of_collects_all_errors(self) -> None:
        doc = _length_of_doc()
        fields = doc["types"][1]["fields"]
        fields[0]["type"] = "float32"
        fields[1]["count"] = 0
        fields.append({"name": "aliases", "type": "uint8", "count": 2,
                       "length_of": "missing"})
        raw = json.dumps(doc).replace(
            '"length_of": "n"', '"length_of": "missing", "length_of": "n"', 1)
        self._assert_raw_rejected(
            raw,
            "types[1].fields[1]: duplicate key: 'length_of'",
            "types[1].fields[1]: count must be an integer >= 1, got 0",
            "types[1].fields[1]: length_of requires an array field with count > 1",
            "types[1].fields[1]: length_of must reference a scalar integer field",
            "types[1].fields[2]: length_of references unknown field 'missing'",
        )

    @staticmethod
    def _mutate_length_of(doc: dict, rng: random.Random) -> int:
        fields = doc["types"][1]["fields"]
        kind = rng.randrange(8)
        if kind == 0:
            fields[1]["length_of"] = rng.choice(
                (None, True, 1.5, [], {"field": "n"}, [["n"]]))
        elif kind == 1:
            fields[1]["length_of"] = rng.choice(
                ("", "9n", "nested.n", "missing", "targets"))
        elif kind == 2:
            fields[1]["count"] = rng.choice(
                (False, True, -1, 0, 1, 1.5, "2", [], {},
                 gen_contracts.MAX_ARRAY_COUNT + 1))
        elif kind == 3:
            fields[0]["type"] = rng.choice(
                ("float32", "bool", "Target_t", ["uint32"], {"scalar": "uint32"}))
        elif kind == 4:
            fields[0]["count"] = rng.choice((0, 2, True, [], "2"))
        elif kind == 5:
            fields[1]["name"] = rng.choice(("for", "__targets", "n", 7, ["targets"]))
        elif kind == 6:
            fields[1]["type"] = rng.choice(("Missing_t", "", ["Target_t"], 7))
        else:
            fields[0]["name"] = rng.choice(("bad-name", 3, ["n"]))
        return kind

    def test_length_of_seeded_declaration_mutations(self) -> None:
        """Sixteen deterministic single mutations, all rejected without output."""
        rng = random.Random(20261029)
        for i in range(16):
            doc = _length_of_doc()
            kind = self._mutate_length_of(doc, rng)
            with self.subTest(mutation=i, kind=kind):
                self._assert_raw_rejected(json.dumps(doc), "types[1].fields[")

    # ---- collect-all / no partial output ---------------------------------

    def test_reports_all_violations_in_one_run(self) -> None:
        doc = _minimal_doc()
        doc["bogus"] = 1
        doc["types"][0]["fields"].append({"name": "x", "type": "uint16"})
        doc["types"][1]["fields"][0]["type"] = "Missing_t"
        doc["types"][1]["fields"][0]["count"] = 0
        self._assert_rejected(
            doc,
            "unknown top-level key: 'bogus'",
            "types[0].fields[1]: duplicate field name: 'x'",
            "types[1].fields[0]: type 'Missing_t' is not a primitive or a "
            "previously-defined type",
            "types[1].fields[0]: count must be an integer >= 1, got 0",
        )

    def test_invalid_document_writes_no_output(self) -> None:
        doc = self._one_type_doc(name="class")
        with tempfile.TemporaryDirectory() as tmp:
            infile = Path(tmp) / "d.json"
            infile.write_text(json.dumps(doc), encoding="utf-8")
            out = Path(tmp) / "out"
            proc = run_cli("--in", str(infile), "--out", str(out), "--force")
            self.assertEqual(proc.returncode, 2)
            self.assertIn("types[0]: name 'class' is a C++ keyword", proc.stderr)
            self.assertFalse(out.exists(), "validation failure wrote output")

    # ---- fuzz-lite --------------------------------------------------------

    @staticmethod
    def _mutate(doc: dict, rng: random.Random) -> None:
        """Apply exactly one structural mutation to a known-good document."""
        types = doc["types"]
        kind = rng.randrange(10)
        if kind == 0:
            doc[rng.choice(("bogus", "extra", "schema"))] = rng.choice(
                (1, None, [], {}))
        elif kind == 1:
            rng.choice(rng.choice(types)["fields"])["type"] = rng.choice(
                ("no_such_t", "class", "", 7, None))
        elif kind == 2:
            rng.choice(rng.choice(types)["fields"])["name"] = rng.choice(
                ("for", "__x", "A_t", 3))
        elif kind == 3:
            rng.choice(rng.choice(types)["fields"])["count"] = rng.choice(
                (-1, 0, 1 << 40, -(1 << 50), 1.5, "4", True))
        elif kind == 4:
            types.append(copy.deepcopy(rng.choice(types)))
        elif kind == 5:
            rng.choice(types)["fields"] = rng.choice(([], {}, 3, "x"))
        elif kind == 6:
            rng.choice(types)["bytes"] = rng.choice((-1, 1.5, "8", 1 << 40))
        elif kind == 7:
            rng.choice(types)["name"] = rng.choice(
                ("class", "FrameHeader", "__bad", 42))
        elif kind == 8:
            doc["types"] = []
        else:
            types.append({"name": f"Extra{len(types)}_t",
                          "fields": [{"name": "v", "type": "uint64"}]})

    def test_fuzz_lite_mutations(self) -> None:
        """Seeded single-mutation fuzz: exit is always 0 or 2, never a
        traceback, and a rejected document leaves no output behind.

        24 CLI runs of a tiny document (~1.2 s). Valid mutations exercise the
        write path, invalid ones the collect-all validator."""
        base = json.loads((TESTDATA / "sample.json").read_text(encoding="utf-8"))
        rng = random.Random(20261024)
        with tempfile.TemporaryDirectory() as tmp:
            for i in range(24):
                doc = copy.deepcopy(base)
                self._mutate(doc, rng)
                infile = Path(tmp) / f"m{i}.json"
                infile.write_text(json.dumps(doc), encoding="utf-8")
                out = Path(tmp) / f"out{i}"
                proc = run_cli("--in", str(infile), "--out", str(out), "--force")
                self.assertIn(proc.returncode, (0, 2), proc.stderr)
                self.assertNotIn("Traceback", proc.stderr)
                if proc.returncode == 2:
                    self.assertTrue(proc.stderr.startswith("gen_contracts: error: "))
                    self.assertFalse(out.exists(),
                                     f"mutation {i} wrote partial output")
                else:
                    self.assertTrue(
                        (out / "include" / "aeb" / "contracts" / "types.h").exists())


if __name__ == "__main__":
    unittest.main()
