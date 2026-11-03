#!/usr/bin/env python3
"""Unit tests for the CI-04 coverage export and per-library gate."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_coverage as cc  # noqa: E402


class CoverageTests(unittest.TestCase):
    def test_exporter_path_exists(self) -> None:
        self.assertTrue(cc.EXPORTER.is_file(), cc.EXPORTER)

    def test_lcov_parser(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            export = pathlib.Path(tmp) / "coverage.info"
            export.write_text(
                f"SF:{cc.ROOT / 'src/runtime/example.cpp'}\n"
                "DA:1,1\nDA:2,0\nDA:3,2\nend_of_record\n",
                encoding="utf-8",
            )
            self.assertEqual(cc.load_export(export),
                             {"src/runtime/example.cpp": (2, 3)})

    def test_llvm_parser(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            export = pathlib.Path(tmp) / "coverage.json"
            export.write_text(json.dumps({
                "data": [{
                    "files": [{
                        "filename": str(cc.ROOT / "src/runtime/example.cpp"),
                        "summary": {"lines": {"covered": 4, "count": 5}},
                    }],
                }],
            }), encoding="utf-8")
            self.assertEqual(cc.load_export(export),
                             {"src/runtime/example.cpp": (4, 5)})

    def test_cmake_source_inventory(self) -> None:
        sources = cc.library_sources(cc.ROOT / "CMakeLists.txt")
        self.assertEqual(set(sources), set(cc.load_thresholds(
            cc.DEFAULT_THRESHOLDS)))
        self.assertIn("src/record/quota_state.cpp", sources["thyme_recorder"])

    def test_gate_accepts_complete_export_and_rejects_missing_source(self) -> None:
        sources = cc.library_sources(cc.ROOT / "CMakeLists.txt")
        entries = [
            f"SF:{cc.ROOT / source}\nDA:1,1\nend_of_record\n"
            for files in sources.values() for source in files
        ]
        with tempfile.TemporaryDirectory() as tmp:
            export = pathlib.Path(tmp) / "coverage.info"
            export.write_text("".join(entries), encoding="utf-8")
            command = [sys.executable, str(cc.ROOT / "tools/ci/check_coverage.py"),
                       "--export", str(export)]
            passed = subprocess.run(command, capture_output=True, text=True,
                                    check=False)
            self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
            self.assertIn("PASS", passed.stdout)

            export.write_text("".join(entries[1:]), encoding="utf-8")
            rejected = subprocess.run(command, capture_output=True, text=True,
                                      check=False)
            self.assertEqual(rejected.returncode, 1,
                             rejected.stdout + rejected.stderr)
            self.assertIn("no coverage data", rejected.stderr)


if __name__ == "__main__":
    unittest.main()
