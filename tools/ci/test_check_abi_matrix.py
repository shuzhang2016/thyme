#!/usr/bin/env python3
"""Unit tests for tools/ci/check_abi_matrix.py (ABI-07).

Fixtures are a throwaway tree with the version-bearing headers and an
ARCHITECTURE.md-shaped matrix, so the tests exercise the parser and drift
detection without depending on the real docs. Registered as
``test_check_abi_matrix`` (labels ``unit;mr_gate``) in the root CMakeLists.
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_abi_matrix as cam  # noqa: E402

HEADER_TEXT = {
    "include/thyme/app/mp_control.h":
        "static constexpr uint32_t kMpControlAbiVersion = 2;\n",
    "include/thyme/runtime/mp_session_identity_page.h":
        "inline constexpr uint32_t kMpSessionIdentityVersion = 1;\n",
    "include/thyme/runtime/session_lease.h":
        "inline constexpr uint32_t kSessionLeaseVersion = 1;\n",
    "include/thyme/runtime/shared_health.h":
        "static constexpr uint32_t kVersion = 2;\n",
    "include/thyme/runtime/pinned_slot_store.h":
        "inline constexpr uint32_t kPinnedSlotStoreVersion = 3;\n",
    "include/thyme/runtime/pinned_slot_lease.h":
        "inline constexpr uint32_t kPinnedSlotLeaseVersion = 4;\n",
    "include/thyme/runtime/shm_spsc_ring.h":
        "inline constexpr uint32_t kShmSpscRingVersion = 3;\n",
    "include/thyme/runtime/shm_spmc_ring.h":
        "inline constexpr uint32_t kShmSpmcRingVersion = 2;\n",
    "include/thyme/runtime/latest_slot.h":
        "inline constexpr uint32_t kLatestSlotLayoutVersion = 2;\n",
    "include/thyme/runtime/record_mirror.h":
        "static constexpr uint32_t kVersion = 2;\n",
    "include/thyme/runtime/record_mirror_ring.h":
        "static constexpr uint32_t kVersion = 3;\n",
    "include/thyme/record/thyme_bin.h":
        "inline constexpr uint16_t kThymeBinFileAbiVersion = 2;\n"
        "inline constexpr uint16_t kThymeBinIndexVersion = 1;\n",
    "include/thyme/record/trigger_protocol.h":
        "inline constexpr uint16_t kTriggerVersion = 3;\n",
    "include/thyme/record/package_integrity.h":
        "inline constexpr uint32_t kManifestVersion = 1;\n",
}

# PinnedSlot channel and lease live in different headers now; their fixture
# versions are fixed independently below (channel 3, lease 4).
ROWS = (
    ("app", "MpControl", "kMpControlAbiVersion", 2),
    ("app", "MpSessionIdentityPage", "kMpSessionIdentityVersion", 1),
    ("app", "SessionLeasePage", "kSessionLeaseVersion", 1),
    ("app", "SharedHealthPage", "SharedHealthPage::kVersion", 2),
    ("pinned_slot", "PinnedSlotPageHeader", "kPinnedSlotStoreVersion", 3),
    ("pinned_slot", "PinnedSlotLeaseHeader", "kPinnedSlotLeaseVersion", 4),
    ("ring", "ShmSpscRing", "kShmSpscRingVersion", 3),
    ("ring", "ShmSpmcRing", "kShmSpmcRingVersion", 2),
    ("latest_slot", "LatestSlot<T>", "kLatestSlotLayoutVersion", 2),
    ("record", "RecordMirrorPage", "RecordMirrorPage::kVersion", 2),
    ("record", "RecordMirrorRingPage", "RecordMirrorRingPage::kVersion", 3),
    ("record", ".thyme.bin", "kThymeBinFileAbiVersion", 2),
    ("record", ".thyme.bin footer index", "kThymeBinIndexVersion", 1),
    ("trigger", "TriggerRequest/TriggerResponse", "kTriggerVersion", 3),
    ("evidence", "integrity.manifest", "kManifestVersion", 1),
)


def _matrix(rows=ROWS, drop: tuple[str, ...] = ()) -> str:
    body = "\n".join(
        f"| {component} | {obj} | {constant} | {version} | shm page | rule 6 |"
        for component, obj, constant, version in rows
        if obj not in drop)
    return (
        "<!-- abi-matrix:begin -->\n"
        "| Component | Object | Constant | Version | Surface | Rule |\n"
        "|---|---|---|---|---|---|\n"
        f"{body}\n"
        "| perf | perf log | — | — | file | no version word |\n"
        "<!-- abi-matrix:end -->\n")


class Fixture:
    def __init__(self, matrix: str | None = None,
                 header_overrides: dict[str, str] | None = None,
                 golden: dict[str, int] | None = None) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        for rel, text in HEADER_TEXT.items():
            _write(self.root / rel, text)
        if header_overrides:
            for rel, value in header_overrides.items():
                _write(self.root / rel, value)
        _write(self.root / "ARCHITECTURE.md",
               matrix if matrix is not None else _matrix())
        if golden is not None:
            _write(self.root / "tools/ops/upgrade/abi_matrix.golden.json",
                   json.dumps({"schema": 1, "entries": [
                       {"constant": constant, "version": version}
                       for constant, version in golden.items()]}))

    def __enter__(self) -> "Fixture":
        return self

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()


def _write(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class MatrixCheckTests(unittest.TestCase):
    def test_matching_matrix_passes(self) -> None:
        with Fixture() as fixture:
            errors, checked = cam.check(
                fixture.root, fixture.root / "ARCHITECTURE.md", None)
            self.assertEqual(errors, [])
            self.assertEqual(checked, len(ROWS))

    def test_header_bump_fails(self) -> None:
        with Fixture(header_overrides={
                "include/thyme/runtime/shm_spsc_ring.h":
                    "inline constexpr uint32_t kShmSpscRingVersion = 4;\n"}) as fixture:
            errors, _ = cam.check(
                fixture.root, fixture.root / "ARCHITECTURE.md", None)
            self.assertTrue(any("ShmSpscRing says v3" in error for error in errors))

    def test_missing_required_row_fails(self) -> None:
        with Fixture(matrix=_matrix(drop=("LatestSlot<T>",))) as fixture:
            errors, _ = cam.check(
                fixture.root, fixture.root / "ARCHITECTURE.md", None)
            self.assertTrue(any("missing required row 'LatestSlot<T>'" in error
                                for error in errors))

    def test_unknown_constant_fails(self) -> None:
        rows = ROWS + (("x", "Mystery", "kMysteryVersion", 1),)
        with Fixture(matrix=_matrix(rows=rows)) as fixture:
            errors, _ = cam.check(
                fixture.root, fixture.root / "ARCHITECTURE.md", None)
            self.assertTrue(any("unknown ABI constant 'kMysteryVersion'" in error
                                for error in errors))

    def test_missing_markers_fails(self) -> None:
        with Fixture(matrix="# no matrix here\n") as fixture:
            errors, _ = cam.check(
                fixture.root, fixture.root / "ARCHITECTURE.md", None)
            self.assertTrue(any("missing" in error and "markers" in error
                                for error in errors))

    def test_non_integer_version_fails(self) -> None:
        matrix = _matrix().replace("| 2 | shm page |", "| two | shm page |", 1)
        with Fixture(matrix=matrix) as fixture:
            errors, _ = cam.check(
                fixture.root, fixture.root / "ARCHITECTURE.md", None)
            self.assertTrue(any("not an integer" in error for error in errors))

    def test_golden_mismatch_fails(self) -> None:
        with Fixture(golden={"kShmSpscRingVersion": 9}) as fixture:
            errors, _ = cam.check(
                fixture.root, fixture.root / "ARCHITECTURE.md",
                fixture.root / "tools/ops/upgrade/abi_matrix.golden.json")
            self.assertTrue(any("golden.json says v9" in error for error in errors))


class CliTests(unittest.TestCase):
    def _cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        script = pathlib.Path(__file__).resolve().parent / "check_abi_matrix.py"
        return subprocess.run([sys.executable, str(script), *args],
                              capture_output=True, text=True, check=False)

    def test_cli_ok(self) -> None:
        with Fixture() as fixture:
            proc = self._cli("--root", str(fixture.root), "--no-golden")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("ABI matrix check OK", proc.stdout)

    def test_cli_drift(self) -> None:
        with Fixture(matrix=_matrix(drop=("MpControl",))) as fixture:
            proc = self._cli("--root", str(fixture.root), "--no-golden")
            self.assertEqual(proc.returncode, 1)
            self.assertIn("missing required row", proc.stderr)


if __name__ == "__main__":
    unittest.main()
