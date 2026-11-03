#!/usr/bin/env python3
"""Unit tests for tools/ci/check_regression_corpus.py (TST-05).

Each test builds a throwaway git repository with real commits, so the scan
exercises ``git log --name-only`` rather than a mocked commit list. The corpus
fixture is the same five-column Markdown the real document uses. Registered as
``test_check_regression_corpus`` (labels ``unit;mr_gate``) in the root
CMakeLists.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_regression_corpus as crc  # noqa: E402


def _git(root: pathlib.Path, *args: str) -> str:
    proc = subprocess.run(["git", "-C", str(root), *args],
                          capture_output=True, text=True, check=True)
    return proc.stdout


class RepoFixture:
    def __init__(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        _git(self.root, "init", "-q")
        _git(self.root, "config", "user.email", "test@example.invalid")
        _git(self.root, "config", "user.name", "Corpus Test")
        _git(self.root, "config", "commit.gpgsign", "false")
        (self.root / "README.md").write_text("fixture\n", encoding="utf-8")
        _git(self.root, "add", "README.md")
        _git(self.root, "commit", "-q", "-m", "chore: fixture root")

    def commit(self, subject: str, files: dict[str, str]) -> str:
        for rel, text in files.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
            _git(self.root, "add", rel)
        _git(self.root, "commit", "-q", "-m", subject)
        return _git(self.root, "rev-parse", "HEAD").strip()

    def corpus(self, rows: str) -> pathlib.Path:
        path = self.root / "docs/reference/regression-corpus.md"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            f"{crc.BEGIN}\n"
            "| Fix commit | Area | Bug | Regression test | Coverage |\n"
            "|---|---|---|---|---|\n"
            f"{rows}\n"
            f"{crc.END}\n", encoding="utf-8")
        return path

    def check(self, rows: str, limit: int = 250):
        return crc.check(self.root, self.corpus(rows), limit)

    def __enter__(self) -> "RepoFixture":
        return self

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()


def _row(commit: str, test: str = "test_regression") -> str:
    return f"| `{commit[:7]}` | runtime | bug | {test} | covered |"


class CorpusCheckTests(unittest.TestCase):
    def test_covered_fix_commit_passes(self) -> None:
        with RepoFixture() as repo:
            commit = repo.commit("fix(runtime): close the race",
                                 {"src/runtime/x.cpp": "int x;\n"})
            errors, scoped, rows = repo.check(_row(commit))
            self.assertEqual(errors, [])
            self.assertEqual(len(scoped), 1)
            self.assertEqual(rows, 1)

    def test_missing_row_fails(self) -> None:
        with RepoFixture() as repo:
            commit = repo.commit("fix(runtime): close the race",
                                 {"src/runtime/x.cpp": "int x;\n"})
            errors, scoped, _ = repo.check(
                _row("0" * 7, "test_unrelated"))
            self.assertEqual(len(scoped), 1)
            self.assertTrue(any(commit[:7] in error and "missing corpus row"
                                in error for error in errors))

    def test_include_only_fix_is_scoped(self) -> None:
        with RepoFixture() as repo:
            commit = repo.commit("fix(abi): bump a constant",
                                 {"include/thyme/x.h": "inline int x = 1;\n"})
            errors, _, _ = repo.check(_row(commit))
            self.assertEqual(errors, [])

    def test_tools_only_fix_is_not_scoped(self) -> None:
        with RepoFixture() as repo:
            repo.commit("fix(gen_graph): scaffold read",
                        {"tools/gen/gen_graph/scaffold/src/x.cpp": "int x;\n"})
            errors, scoped, _ = repo.check("")
            self.assertEqual(errors, [])
            self.assertEqual(scoped, [])

    def test_feat_commit_is_not_scoped(self) -> None:
        with RepoFixture() as repo:
            repo.commit("feat(runtime): add a thing",
                        {"src/runtime/x.cpp": "int x;\n"})
            errors, scoped, _ = repo.check("")
            self.assertEqual(errors, [])
            self.assertEqual(scoped, [])

    def test_no_test_marker_is_allowed(self) -> None:
        with RepoFixture() as repo:
            commit = repo.commit("fix(record): fsync a directory",
                                 {"src/record/x.cpp": "int x;\n"})
            errors, _, _ = repo.check(_row(
                commit,
                "<!-- no-test: directory fsync is not observable in-process -->"))
            self.assertEqual(errors, [])

    def test_empty_no_test_reason_fails(self) -> None:
        with RepoFixture() as repo:
            commit = repo.commit("fix(record): fsync a directory",
                                 {"src/record/x.cpp": "int x;\n"})
            errors, _, _ = repo.check(_row(commit, "<!-- no-test: -->"))
            self.assertTrue(any("needs a reason" in error for error in errors))

    def test_unterminated_no_test_marker_fails(self) -> None:
        with RepoFixture() as repo:
            commit = repo.commit("fix(record): fsync a directory",
                                 {"src/record/x.cpp": "int x;\n"})
            errors, _, _ = repo.check(_row(commit, "<!-- no-test: no reason"))
            self.assertTrue(any("malformed no-test marker" in error
                                for error in errors))

    def test_empty_test_cell_fails(self) -> None:
        with RepoFixture() as repo:
            commit = repo.commit("fix(runtime): close the race",
                                 {"src/runtime/x.cpp": "int x;\n"})
            errors, _, _ = repo.check(
                f"| `{commit[:7]}` | runtime | bug | | covered |")
            self.assertTrue(any("missing regression test" in error
                                for error in errors))

    def test_bad_hash_fails(self) -> None:
        with RepoFixture() as repo:
            repo.commit("fix(runtime): close the race",
                        {"src/runtime/x.cpp": "int x;\n"})
            errors, _, _ = repo.check("| not-a-hash | runtime | bug | test_x | ok |")
            self.assertTrue(any("not a 7-40 char hash" in error
                                for error in errors))

    def test_missing_markers_fail(self) -> None:
        with RepoFixture() as repo:
            path = repo.root / "docs/reference/regression-corpus.md"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# no corpus\n", encoding="utf-8")
            errors, _, _ = crc.check(repo.root, path)
            self.assertTrue(any("markers" in error for error in errors))

    def test_limit_ignores_commit_outside_window(self) -> None:
        with RepoFixture() as repo:
            repo.commit("fix(runtime): old bug",
                        {"src/runtime/old.cpp": "int old;\n"})
            commit = repo.commit("fix(runtime): new bug",
                                 {"src/runtime/new.cpp": "int new;\n"})
            errors, scoped, _ = repo.check(_row(commit), limit=1)
            self.assertEqual(errors, [])
            self.assertEqual(len(scoped), 1)

    def test_row_for_unscanned_commit_is_accepted(self) -> None:
        with RepoFixture() as repo:
            errors, scoped, rows = repo.check(_row("abc1234"))
            self.assertEqual(errors, [])
            self.assertEqual(scoped, [])
            self.assertEqual(rows, 1)


if __name__ == "__main__":
    unittest.main()
