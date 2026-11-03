#!/usr/bin/env python3
"""DOC-01: executable-doc checks.

Two independent checks, both cheap and deterministic:

1. **Commands** — fenced shell blocks in the first-party docs must reference
   binaries that exist as CMake targets and repository scripts that exist on
   disk. Nothing is executed; only the command word and ``build*/...`` path
   tokens are inspected. Paths that are obviously artifacts (``.json``,
   ``.tbin``, ...) are ignored, and a block can opt out with
   ``<!-- doc-check: ignore -->``.

2. **Test counts** — numbers claimed in ``docs/guides/test-matrix.md`` and
   ``docs/guides/onboarding.md`` must equal a CTest listing of a default configure
   (``--build-dir`` or a pre-generated ``--ctest-json``). Without one of
   those options the count check is skipped and says so.

Exit code 0 means every enabled check passed.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

SKIP_MARKER = re.compile(r"<!--\s*doc-check:\s*ignore")
SHELL_LANGS = {"", "bash", "sh", "shell", "console", "zsh"}
FENCE = re.compile(r"^\s*```(\w*)")
ARTIFACT_SUFFIXES = {
    "json", "md", "txt", "yaml", "yml", "ini", "log", "tbin", "manifest",
    "pb", "csv", "html", "js", "css", "png", "svg", "pdf", "xml", "profraw",
}
BUILD_SEGMENT = re.compile(r"(?:^|/)build[A-Za-z0-9_.-]*/")
REPO_PREFIX = re.compile(r"^(?:\./)?(?:tools|scripts|deploy)/")
ENV_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
SEGMENT_SPLIT = re.compile(r"&&|\|\||;|\|")
# Shell keywords / wrappers can precede the real command word.
WRAPPERS = {"sudo", "env", "time", "nohup", "command", "exec"}
KEYWORDS = {
    "if", "then", "else", "elif", "fi", "for", "do", "done", "while", "until",
    "case", "esac", "!", "true", "false", "exit", "cd", "export", "set",
    "unset", "echo", "printf", "mkdir", "rm", "ls", "cat", "tail", "sed",
    "chmod", "cp", "mv", "diff", "grep", "test",
}
TARGET_CALL = re.compile(r"\badd_executable\s*\(\s*([A-Za-z0-9_.+-]+)")

# (doc, claim regex, kind). Each spec must match at least once, so rewording a
# claim forces the tooling to be updated instead of silently disabling it.
COUNT_CLAIMS = (
    ("docs/guides/test-matrix.md",
     re.compile(r"\*\*(\d+) ctest cases\*\*"), "total"),
    ("docs/guides/test-matrix.md",
     re.compile(r"(\d+) of them labelled"), "mr_gate"),
    ("docs/guides/test-matrix.md",
     re.compile(r"`ctest -L mr_gate`\s*\((\d+) cases"), "mr_gate"),
    ("docs/guides/test-matrix.md",
     re.compile(r"full `ctest`\s*\((\d+) cases"), "total"),
    ("docs/guides/onboarding.md",
     re.compile(r"fast PR gate\s*\(~?(\d+) cases\)"), "mr_gate"),
)


def doc_paths(root: pathlib.Path) -> list[pathlib.Path]:
    paths = [root / "README.md", root / "ARCHITECTURE.md"]
    paths.extend(sorted((root / "docs").rglob("*.md")))
    return [path for path in paths if path.is_file()]


def cmake_targets(root: pathlib.Path) -> set[str]:
    files = []
    for current, dirs, names in os.walk(root, onerror=lambda _error: None):
        dirs[:] = sorted(
            name for name in dirs if not name.startswith(("build", ".git")))
        if "CMakeLists.txt" in names:
            files.append(pathlib.Path(current) / "CMakeLists.txt")
    targets: set[str] = set()
    for path in sorted(files):
        try:
            targets.update(TARGET_CALL.findall(
                path.read_text(encoding="utf-8", errors="replace")))
        except FileNotFoundError:
            continue
    return targets


def fenced_blocks(text: str):
    """Yield ``(opening_line, block)`` for shell-ish fenced blocks.

    A fence must be tracked with open/close state: a closing ``` after a
    non-shell block would otherwise look like an opening fence and swallow the
    prose that follows.
    """
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        match = FENCE.match(lines[index])
        if match is None:
            index += 1
            continue
        language = match.group(1).lower()
        start = index + 1
        block: list[tuple[int, str]] = []
        index += 1
        while index < len(lines) and FENCE.match(lines[index]) is None:
            block.append((index + 1, lines[index]))
            index += 1
        if language in SHELL_LANGS:
            yield start, block
        index += 1


def block_is_ignored(lines: list[str], fence_line: int,
                     block: list[tuple[int, str]]) -> bool:
    if any(SKIP_MARKER.search(line) for _, line in block):
        return True
    for line in reversed(lines[:fence_line - 1]):
        if not line.strip():
            continue
        return bool(SKIP_MARKER.search(line))
    return False


def logical_commands(block: list[tuple[int, str]]):
    pending = ""
    start = 0
    for line_no, raw in block:
        line = re.sub(r"\s+#.*$", "", raw.rstrip())
        stripped = line.strip()
        if not pending:
            if not stripped or stripped.startswith("#"):
                continue
            start = line_no
        pending += (" " if pending else "") + stripped.rstrip("\\").strip()
        if raw.rstrip().endswith("\\"):
            continue
        yield start, pending
        pending = ""
    if pending:
        yield start, pending


def check_command(root: pathlib.Path, doc: pathlib.Path, line_no: int,
                  segment: str, targets: set[str]) -> list[str]:
    errors: list[str] = []
    tokens = segment.split()
    while tokens and (ENV_ASSIGNMENT.match(tokens[0]) or tokens[0] in WRAPPERS):
        tokens.pop(0)
    if not tokens or tokens[0] in KEYWORDS:
        return errors

    for token in tokens:
        candidate = token.strip("'\"`,()")
        if candidate.startswith("--"):
            continue
        if candidate.endswith((".py", ".sh")):
            rel = candidate[2:] if candidate.startswith("./") else candidate
            if REPO_PREFIX.match(candidate):
                top = rel.split("/", 1)[0]
                if (root / top).exists() and not (root / rel).exists():
                    errors.append(f"{doc}:{line_no}: script not found: {rel}")
            continue
        if "=" in candidate:
            candidate = candidate.rsplit("=", 1)[-1]
        if any(ch in candidate for ch in "$<*"):
            continue
        if not BUILD_SEGMENT.search(candidate):
            continue
        if candidate.endswith("/"):
            continue
        base = candidate.rstrip("/").rsplit("/", 1)[-1]
        if base.startswith("...") or base in {"...", "…"}:
            continue
        if "." in base and base.rsplit(".", 1)[-1] in ARTIFACT_SUFFIXES:
            continue
        if base not in targets:
            errors.append(
                f"{doc}:{line_no}: '{candidate}' is not an add_executable "
                f"target (base '{base}')")
    return errors


def check_commands(root: pathlib.Path) -> tuple[list[str], int, int]:
    targets = cmake_targets(root)
    errors: list[str] = []
    blocks = 0
    commands = 0
    for doc in doc_paths(root):
        try:
            text = doc.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            continue
        lines = text.splitlines()
        rel = doc.relative_to(root).as_posix()
        for fence_line, block in fenced_blocks(text):
            blocks += 1
            if block_is_ignored(lines, fence_line, block):
                continue
            for line_no, command in logical_commands(block):
                for segment in SEGMENT_SPLIT.split(command):
                    segment = segment.strip()
                    if not segment:
                        continue
                    commands += 1
                    errors.extend(check_command(
                        root, rel, line_no, segment, targets))
    return errors, blocks, commands


def _counts_from_doc(doc: dict) -> tuple[int, int]:
    tests = doc.get("tests", [])
    mr_gate = 0
    for test in tests:
        for prop in test.get("properties", []):
            if prop.get("name") == "LABELS" and "mr_gate" in prop.get("value", []):
                mr_gate += 1
                break
    return len(tests), mr_gate


def ctest_counts(ctest_json: pathlib.Path) -> tuple[int, int]:
    return _counts_from_doc(json.loads(ctest_json.read_text(encoding="utf-8")))


def counts_from_build_dir(build_dir: pathlib.Path,
                          runner=subprocess.run) -> tuple[int, int]:
    proc = runner(
        ["ctest", "--test-dir", str(build_dir), "-N", "--show-only=json-v1"],
        capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(
            f"ctest --test-dir {build_dir} -N failed: "
            f"{(proc.stderr or proc.stdout).strip()}")
    return _counts_from_doc(json.loads(proc.stdout))


def check_counts(root: pathlib.Path, total: int, mr_gate: int) -> list[str]:
    errors: list[str] = []
    actual = {"total": total, "mr_gate": mr_gate}
    matched: set[tuple[str, str]] = set()
    for rel, pattern, kind in COUNT_CLAIMS:
        path = root / rel
        if not path.is_file():
            errors.append(f"{rel}: claim file not found")
            continue
        for line_no, line in enumerate(
                path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if SKIP_MARKER.search(line):
                continue
            for value in pattern.findall(line):
                matched.add((rel, pattern.pattern))
                claimed = int(value)
                if claimed != actual[kind]:
                    errors.append(
                        f"{rel}:{line_no}: claims {claimed} {kind} cases, "
                        f"default configure has {actual[kind]}")
    for rel, pattern, _kind in COUNT_CLAIMS:
        if (rel, pattern.pattern) not in matched:
            errors.append(
                f"{rel}: test-count claim no longer matches "
                f"/{pattern.pattern}/ (update check_doc_commands.py)")
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Check doc commands against CMake targets and doc test "
                    "counts against a CTest listing")
    parser.add_argument("--root", default=str(ROOT),
                        help="repository root (default: this checkout)")
    parser.add_argument("--build-dir",
                        help="configured build tree to list tests from")
    parser.add_argument("--ctest-json",
                        help="pre-generated `ctest -N --show-only=json-v1`")
    parser.add_argument("--no-commands", action="store_true",
                        help="skip the fenced-command check")
    parser.add_argument("--no-counts", action="store_true",
                        help="skip the test-count check")
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root).resolve()

    errors: list[str] = []
    summary = []
    if not args.no_commands:
        command_errors, blocks, commands = check_commands(root)
        errors.extend(command_errors)
        summary.append(f"{commands} commands in {blocks} fenced blocks")

    counts_enabled = not args.no_counts and (
        args.build_dir is not None or args.ctest_json is not None)
    if counts_enabled:
        try:
            if args.ctest_json is not None:
                total, mr_gate = ctest_counts(pathlib.Path(args.ctest_json))
            else:
                total, mr_gate = counts_from_build_dir(
                    pathlib.Path(args.build_dir))
        except (OSError, RuntimeError, json.JSONDecodeError) as exc:
            errors.append(f"cannot read CTest listing: {exc}")
        else:
            errors.extend(check_counts(root, total, mr_gate))
            summary.append(f"{total} ctest cases ({mr_gate} mr_gate)")
    elif not args.no_counts:
        print("doc command check: test counts skipped "
              "(pass --build-dir or --ctest-json)", file=sys.stderr)

    if errors:
        print("doc command check failed:", file=sys.stderr)
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("doc command check OK" +
          (f" ({', '.join(summary)})" if summary else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
