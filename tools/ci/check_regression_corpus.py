#!/usr/bin/env python3
"""TST-05: every scoped ``fix(...)`` commit must have a regression-corpus row.

The corpus lives in ``docs/reference/regression-corpus.md`` between the markers::

    <!-- regression-corpus:begin -->
    | Fix commit | Area | Bug | Regression test | Coverage |
    ...
    <!-- regression-corpus:end -->

A row covers a commit when its first column is a 7-40 character commit hash
prefix of that commit. The ``Regression test`` cell names the ctest case (or
fuzz target) that would fail if the bug came back. A fix that genuinely has no
test must say so explicitly::

    | `abc1234` | record | ... | <!-- no-test: seal-only fsync, not observable in-process --> | no test |

Scanning is bounded to the most recent ``--limit`` commits (default 250, the
same window the corpus documents). Only ``fix(...)``/``fix:`` subjects that
touch ``src/`` or ``include/`` are required; changes confined to tools, tests
or docs are recorded when useful but are not gated. Rows for commits older
than the scan window are accepted (they are not re-validated on every run).
"""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BEGIN = "<!-- regression-corpus:begin -->"
END = "<!-- regression-corpus:end -->"
DEFAULT_CORPUS = "docs/reference/regression-corpus.md"
DEFAULT_LIMIT = 250

HASH = re.compile(r"^[0-9a-f]{7,40}$")
SEPARATOR = re.compile(r"^:?-{3,}:?$")
FIX_SUBJECT = re.compile(r"^fix(?:\([^)]*\))?:")
NO_TEST = re.compile(r"<!--\s*no-test:\s*(.*?)-->")
SCOPE_PREFIXES = ("src/", "include/")


class Row:
    __slots__ = ("commit", "test", "line")

    def __init__(self, commit: str, test: str, line: int) -> None:
        self.commit = commit
        self.test = test
        self.line = line


def split_row(line: str) -> list[str]:
    return [cell.replace("`", "").strip()
            for cell in line.strip().strip("|").split("|")]


def parse_corpus(text: str, doc_name: str) -> tuple[list[Row], list[str]]:
    errors: list[str] = []
    if BEGIN not in text or END not in text:
        return [], [f"{doc_name}: missing {BEGIN} / {END} markers"]
    body = text.split(BEGIN, 1)[1].split(END, 1)[0]
    rows: list[Row] = []
    for line_no, line in enumerate(body.splitlines(), 1):
        if not line.lstrip().startswith("|"):
            continue
        cells = split_row(line)
        if not cells or cells[0].lower() in ("fix commit", "commit"):
            continue
        if all(SEPARATOR.match(cell.replace(" ", "")) or cell == ""
               for cell in cells):
            continue
        if len(cells) < 5:
            errors.append(
                f"{doc_name}:{line_no}: expected 5 columns, got {len(cells)}")
            continue
        commit = cells[0]
        if not HASH.match(commit):
            errors.append(
                f"{doc_name}:{line_no}: '{commit}' is not a 7-40 char hash")
            continue
        test = cells[3]
        marker = NO_TEST.search(test)
        if marker is not None:
            if not marker.group(1).strip():
                errors.append(
                    f"{doc_name}:{line_no}: no-test marker needs a reason")
            rows.append(Row(commit, test, line_no))
            continue
        if "no-test" in test.lower() or test.lstrip().startswith("<!--"):
            errors.append(
                f"{doc_name}:{line_no}: malformed no-test marker for {commit}")
            continue
        if test in ("", "—", "-"):
            errors.append(
                f"{doc_name}:{line_no}: missing regression test for {commit}")
            continue
        rows.append(Row(commit, test, line_no))
    return rows, errors


def _git(repo: pathlib.Path, *args: str) -> str:
    proc = subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise SystemExit(
            f"git {' '.join(args)} failed in {repo}: {proc.stderr.strip()}")
    return proc.stdout


def scan_fix_commits(repo: pathlib.Path, limit: int) -> list[dict]:
    """Return fix commits (hash, subject, files) within the scan window."""
    output = _git(repo, "log", f"-n{limit}", "--format=%H%x1f%s",
                  "--name-only")
    commits: list[dict] = []
    current: dict | None = None
    for line in output.splitlines():
        if "\x1f" in line:
            commit_hash, subject = line.split("\x1f", 1)
            current = {"hash": commit_hash, "subject": subject, "files": []}
            commits.append(current)
        elif line.strip() and current is not None:
            current["files"].append(line.strip())
    scoped: list[dict] = []
    for commit in commits:
        if not FIX_SUBJECT.match(commit["subject"]):
            continue
        if any(path.startswith(SCOPE_PREFIXES) for path in commit["files"]):
            scoped.append(commit)
    return scoped


def check(repo: pathlib.Path, corpus: pathlib.Path,
          limit: int = DEFAULT_LIMIT) -> tuple[list[str], list[dict], int]:
    doc_name = str(corpus)
    rows, errors = parse_corpus(
        corpus.read_text(encoding="utf-8"), doc_name)
    try:
        scoped = scan_fix_commits(repo, limit)
    except SystemExit as exc:
        return errors + [str(exc)], [], len(rows)
    prefixes = {row.commit for row in rows}
    missing = [commit for commit in scoped
               if not any(commit["hash"].startswith(prefix)
                          for prefix in prefixes)]
    for commit in missing:
        errors.append(
            "missing corpus row for "
            f"{commit['hash'][:7]} {commit['subject']}")
    return errors, scoped, len(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=str(ROOT),
                        help="git repository to scan (default: repo root)")
    parser.add_argument("--corpus", default=None,
                        help=f"corpus Markdown file (default: <repo>/{DEFAULT_CORPUS})")
    parser.add_argument("--limit", type=int, default=DEFAULT_LIMIT,
                        help=f"commits to scan (default: {DEFAULT_LIMIT})")
    args = parser.parse_args(argv)

    repo = pathlib.Path(args.repo).resolve()
    corpus = (pathlib.Path(args.corpus).resolve() if args.corpus
              else repo / DEFAULT_CORPUS)
    if not corpus.is_file():
        print(f"regression corpus: {corpus} not found", file=sys.stderr)
        return 1

    errors, scoped, row_count = check(repo, corpus, args.limit)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(f"regression corpus: FAIL ({len(errors)} error(s), "
              f"{len(scoped)} scoped fix commit(s) scanned, {row_count} row(s))",
              file=sys.stderr)
        return 1
    print(f"regression corpus: OK ({len(scoped)} scoped fix commit(s) scanned "
          f"in the last {args.limit}, {row_count} row(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
