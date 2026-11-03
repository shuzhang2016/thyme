#!/usr/bin/env python3
"""Unit tests for tools/ci/check_doc_commands.py (DOC-01).

Plain ``unittest`` over a temporary fixture tree: no CMake configure and no
test execution. Registered as ``test_check_doc_commands`` (labels
``unit;mr_gate``) in the root CMakeLists.
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_doc_commands as cdc  # noqa: E402


def _write(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _claims(total: int = 2, mr_gate: int = 1) -> tuple[str, str]:
    matrix = f"""# Test matrix

A default configure builds **{total} ctest cases**, {mr_gate} of them labelled
`mr_gate`.

| Job | Trigger | Suite | Sanitizer | Blocking |
|-----|---------|-------|-----------|----------|
| `test-mr-gate` | MR | `ctest -L mr_gate` ({mr_gate} cases minus quarantined: units) | none | yes |
| `test-linux` | branch | full `ctest` ({total} cases minus quarantined; includes fuzz) | none | yes |
"""
    onboarding = f"```bash\nctest -L mr_gate   # fast PR gate (~{mr_gate} cases)\n```\n"
    return matrix, onboarding


def _ctest_json(total: int = 2, mr_gate: int = 1) -> dict:
    tests = []
    for index in range(total):
        labels = ["unit"]
        if index < mr_gate:
            labels.append("mr_gate")
        tests.append({
            "name": f"test_{index}",
            "properties": [{"name": "LABELS", "value": labels}],
        })
    return {"kind": "ctestInfo", "version": {"major": 1}, "tests": tests}


class Fixture:
    def __init__(self, commands: str = "",
                 commands_marker: bool = False,
                 total: int = 2, mr_gate: int = 1,
                 claims: tuple[int, int] | None = None) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        _write(self.root / "CMakeLists.txt",
               "add_executable(gen_graph main.cpp)\n"
               "add_executable(bench_core bench.cpp)\n")
        _write(self.root / "tools/ci/exists.py", "print('ok')\n")
        _write(self.root / "tools/ops/upgrade/thyme_upgrade.sh", "#!/bin/sh\n")
        matrix, onboarding = _claims(*(claims or (total, mr_gate)))
        _write(self.root / "docs/guides/test-matrix.md", matrix)
        _write(self.root / "docs/guides/onboarding.md", onboarding)
        marker = "<!-- doc-check: ignore -->\n" if commands_marker else ""
        _write(self.root / "docs/commands.md",
               f"# Commands\n\n{marker}```bash\n{commands}\n```\n")
        self.ctest_json = self.root / "ctest.json"
        self.ctest_json.write_text(json.dumps(_ctest_json(total, mr_gate)),
                                   encoding="utf-8")

    def __enter__(self) -> "Fixture":
        return self

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()


class CommandCheckTests(unittest.TestCase):
    def test_valid_commands_pass(self) -> None:
        with Fixture("./build/gen_graph --out /tmp/app\n"
                     "cmake --build build --target gen_graph\n"
                     "python3 tools/ci/exists.py\n") as fixture:
            errors, blocks, commands = cdc.check_commands(fixture.root)
            self.assertEqual(errors, [])
            self.assertGreaterEqual(blocks, 1)
            self.assertGreaterEqual(commands, 3)

    def test_unknown_build_binary_fails(self) -> None:
        with Fixture("./build/no_such_tool --flag\n") as fixture:
            errors, _, _ = cdc.check_commands(fixture.root)
            self.assertEqual(len(errors), 1)
            self.assertIn("no_such_tool", errors[0])
            self.assertIn("not an add_executable target", errors[0])

    def test_artifact_tokens_are_ignored(self) -> None:
        with Fixture("./build/bench_core --json build/bench_core.json\n"
                     "./build/bench_core --json build/report.json\n"
                     "tools/ops/upgrade/thyme_upgrade.sh --probe build/bench_core\n") as fixture:
            errors, _, _ = cdc.check_commands(fixture.root)
            self.assertEqual(errors, [])

    def test_block_opt_out(self) -> None:
        with Fixture("./build/no_such_tool --flag\n",
                     commands_marker=True) as fixture:
            errors, _, _ = cdc.check_commands(fixture.root)
            self.assertEqual(errors, [])

    def test_missing_repo_script_fails(self) -> None:
        with Fixture("python3 tools/ci/missing.py\n") as fixture:
            errors, _, _ = cdc.check_commands(fixture.root)
            self.assertEqual(len(errors), 1)
            self.assertIn("tools/ci/missing.py", errors[0])

    def test_prose_outside_fence_is_ignored(self) -> None:
        with Fixture() as fixture:
            _write(fixture.root / "docs/prose.md",
                   "Use `build/no_such_tool` from the shell.\n")
            errors, _, _ = cdc.check_commands(fixture.root)
            self.assertEqual(errors, [])

    def test_generated_files_may_disappear_during_scan(self) -> None:
        with Fixture() as fixture:
            with mock.patch.object(pathlib.Path, "read_text",
                                   side_effect=FileNotFoundError):
                self.assertEqual(cdc.cmake_targets(fixture.root), set())
                errors, _, _ = cdc.check_commands(fixture.root)
                self.assertEqual(errors, [])

    def test_generated_directories_may_disappear_during_scan(self) -> None:
        with Fixture() as fixture:
            build_dir = fixture.root / "build/fuzz_gen_graph/run/out/include"
            scratch_dir = fixture.root / "generated/vanishing"
            build_dir.mkdir(parents=True)
            scratch_dir.mkdir(parents=True)
            real_scandir = cdc.os.scandir

            def disappear(path):
                if pathlib.Path(path) in {build_dir, scratch_dir}:
                    raise FileNotFoundError(path)
                return real_scandir(path)

            with mock.patch.object(cdc.os, "scandir", side_effect=disappear):
                self.assertEqual(cdc.cmake_targets(fixture.root),
                                 {"gen_graph", "bench_core"})


class CountCheckTests(unittest.TestCase):
    def test_matching_counts_pass(self) -> None:
        with Fixture(total=2, mr_gate=1) as fixture:
            self.assertEqual(
                cdc.check_counts(fixture.root, *cdc.ctest_counts(fixture.ctest_json)),
                [])

    def test_count_mismatch_fails(self) -> None:
        with Fixture(total=2, mr_gate=1, claims=(3, 1)) as fixture:
            errors = cdc.check_counts(fixture.root, 2, 1)
            self.assertTrue(any("claims 3 total" in error for error in errors))

    def test_mr_gate_mismatch_fails(self) -> None:
        with Fixture(total=2, mr_gate=1, claims=(2, 5)) as fixture:
            errors = cdc.check_counts(fixture.root, 2, 1)
            self.assertTrue(any("claims 5 mr_gate" in error for error in errors))

    def test_removed_claim_fails(self) -> None:
        with Fixture() as fixture:
            _write(fixture.root / "docs/guides/onboarding.md", "no counts here\n")
            errors = cdc.check_counts(fixture.root, 2, 1)
            self.assertTrue(any("no longer matches" in error for error in errors))

    def test_count_opt_out(self) -> None:
        with Fixture(total=2, mr_gate=1) as fixture:
            matrix = fixture.root / "docs/guides/test-matrix.md"
            matrix.write_text(
                matrix.read_text(encoding="utf-8") +
                "Extra: **99 ctest cases** <!-- doc-check: ignore -->\n",
                encoding="utf-8")
            self.assertEqual(cdc.check_counts(fixture.root, 2, 1), [])


class CliTests(unittest.TestCase):
    def _cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        script = pathlib.Path(__file__).resolve().parent / "check_doc_commands.py"
        return subprocess.run([sys.executable, str(script), *args],
                              capture_output=True, text=True, check=False)

    def test_cli_ok_with_ctest_json(self) -> None:
        with Fixture() as fixture:
            proc = self._cli("--root", str(fixture.root),
                            "--ctest-json", str(fixture.ctest_json))
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("doc command check OK", proc.stdout)

    def test_cli_fails_on_drift(self) -> None:
        with Fixture("./build/no_such_tool\n") as fixture:
            proc = self._cli("--root", str(fixture.root))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("no_such_tool", proc.stderr)


if __name__ == "__main__":
    unittest.main()
