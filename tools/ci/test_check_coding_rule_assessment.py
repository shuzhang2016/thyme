#!/usr/bin/env python3
"""Negative fixtures for the coding-rule evidence gate."""
from __future__ import annotations

import copy
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_coding_rule_assessment as assessment  # noqa: E402

MAPPING = {
    "schema": 1,
    "standards": [{
        "name": "MISRA C++:2008",
        "status": "not_assessed",
        "reason": "No complete rule-by-rule assessment.",
    }],
    "rules": [{
        "id": "CR-01",
        "claim": "local rule",
        "evidence": [
            {"type": "cmake_flag", "flag": "-fno-exceptions"},
            {"type": "ci", "job": "test-werror", "required_text": "THYME_WERROR=ON"},
            {"type": "clang_tidy", "check": "cppcoreguidelines-no-malloc"},
            {"type": "ctest", "name": "test_core_no_alloc"},
            {"type": "document", "path": "docs/reference/review-checklist.md"},
        ],
        "limit": "selected paths only",
    }],
}
CI = """jobs:
  test-werror:
    steps:
      - run: cmake -DTHYME_WERROR=ON
  check-clang-tidy:
    steps:
      - run: python3 tools/ci/run_clang_tidy.py
"""


def _write(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class Fixture:
    def __init__(self, mapping: object = None, report: str | None = None) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        self.mapping = copy.deepcopy(MAPPING if mapping is None else mapping)
        _write(self.root / assessment.CODING_RULES, "## CR-01 local\n")
        _write(self.root / "CMakeLists.txt", "# -fno-exceptions\n")
        _write(self.root / assessment.CI_FILE, CI)
        _write(self.root / assessment.CLANG_TIDY,
               "Checks: 'cppcoreguidelines-no-malloc'\n")
        _write(self.root / "docs/reference/review-checklist.md", "review\n")
        self.ctest_json = self.root / "ctest.json"
        _write(self.ctest_json, json.dumps({"tests": [{"name": "test_core_no_alloc"}]}))
        self.save_mapping()
        if report is not None:
            _write(self.root / assessment.REPORT, report)

    def __enter__(self) -> "Fixture":
        return self

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()

    def save_mapping(self) -> None:
        _write(self.root / assessment.MAPPING, json.dumps(self.mapping))

    def check(self) -> list[str]:
        errors, _ = assessment.validate(self.root, self.mapping, {"test_core_no_alloc"})
        return errors

    def cli(self, *extra: str) -> subprocess.CompletedProcess[str]:
        script = pathlib.Path(__file__).resolve().with_name("check_coding_rule_assessment.py")
        return subprocess.run(
            [sys.executable, str(script), "--root", str(self.root),
             "--ctest-json", str(self.ctest_json), *extra],
            capture_output=True, text=True, check=False)


class AssessmentTests(unittest.TestCase):
    def test_valid_mapping_generates_bounded_report(self) -> None:
        with Fixture() as fixture:
            self.assertEqual(fixture.check(), [])
            generated = fixture.cli("--write")
            self.assertEqual(generated.returncode, 0, generated.stderr)
            checked = fixture.cli()
            self.assertEqual(checked.returncode, 0, checked.stderr)
            report = (fixture.root / assessment.REPORT).read_text(encoding="utf-8")
            self.assertIn("not_assessed", report)
            self.assertIn("does not mean compliant", report)
            self.assertIn("selected paths only", report)

    def test_external_standard_cannot_be_claimed_assessed(self) -> None:
        with Fixture() as fixture:
            fixture.mapping["standards"][0]["status"] = "compliant"
            errors = fixture.check()
            self.assertTrue(any("must remain not_assessed" in error for error in errors))

    def test_missing_and_orphan_rules_fail(self) -> None:
        with Fixture() as fixture:
            fixture.mapping["rules"] = []
            self.assertTrue(any("rules must be a non-empty list" in error
                                for error in fixture.check()))
        with Fixture() as fixture:
            fixture.mapping["rules"][0]["id"] = "CR-99"
            errors = fixture.check()
            self.assertTrue(any("orphan rule 'CR-99'" in error for error in errors))
            self.assertTrue(any("missing rule 'CR-01'" in error for error in errors))

    def test_unknown_ctest_fails(self) -> None:
        with Fixture() as fixture:
            fixture.mapping["rules"][0]["evidence"][3]["name"] = "test_missing"
            self.assertTrue(any("referenced CTest does not exist" in error
                                for error in fixture.check()))

    def test_duplicate_evidence_fails(self) -> None:
        with Fixture() as fixture:
            evidence = fixture.mapping["rules"][0]["evidence"]
            evidence.append(copy.deepcopy(evidence[0]))
            self.assertTrue(any("duplicate evidence" in error for error in fixture.check()))

    def test_missing_cmake_flag_fails(self) -> None:
        with Fixture() as fixture:
            fixture.mapping["rules"][0]["evidence"][0]["flag"] = "-fno-not-present"
            self.assertTrue(any("CMake flag is absent" in error for error in fixture.check()))

    def test_missing_ci_text_fails(self) -> None:
        with Fixture() as fixture:
            fixture.mapping["rules"][0]["evidence"][1]["required_text"] = "MISSING"
            self.assertTrue(any("lacks 'MISSING'" in error for error in fixture.check()))

    def test_missing_tidy_check_fails(self) -> None:
        with Fixture() as fixture:
            fixture.mapping["rules"][0]["evidence"][2]["check"] = "missing-check"
            self.assertTrue(any("clang-tidy check is absent" in error
                                for error in fixture.check()))

    def test_document_must_stay_in_repository(self) -> None:
        with Fixture() as fixture:
            fixture.mapping["rules"][0]["evidence"][4]["path"] = "../outside.md"
            self.assertTrue(any("inside repository" in error for error in fixture.check()))

    def test_duplicate_json_key_fails(self) -> None:
        with Fixture() as fixture:
            _write(fixture.root / assessment.MAPPING, '{"schema":1,"schema":1}')
            self.assertIn("duplicate JSON key", fixture.cli().stderr)

    def test_stale_report_and_negative_cli_fail(self) -> None:
        with Fixture(report="stale\n") as fixture:
            self.assertIn("stale report", fixture.cli().stderr)
        with Fixture() as fixture:
            fixture.mapping["rules"][0]["evidence"] = []
            fixture.save_mapping()
            proc = fixture.cli()
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("empty evidence", proc.stderr)


if __name__ == "__main__":
    unittest.main()
