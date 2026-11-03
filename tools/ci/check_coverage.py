#!/usr/bin/env python3
"""CI-04: per-library line-coverage gate.

Reads the coverage export produced from a ``-DTHYME_ENABLE_COVERAGE=ON`` tree
(``llvm-cov export`` JSON for clang builds, ``lcov`` .info for gcc builds --
the exporter picks one format from the built tree, never both), attributes
every measured source file to its CMake library target, and compares the
aggregate line coverage against ``tools/ci/coverage_thresholds.json``.

Library coverage is defined over the lines of the target's own ``src/*.cpp``
files; headers are excluded because they are shared between targets and
attributing inline header lines to a single library would be arbitrary.

Exit codes: 0 = every library meets its threshold, 1 = gate failure,
2 = usage/export error (including a library with no measured data).
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_THRESHOLDS = ROOT / "tools" / "ci" / "coverage_thresholds.json"
EXPORTER = ROOT / "tools" / "ops" / "coverage" / "export_coverage.sh"

ADD_LIBRARY = re.compile(r"add_library\(\s*(thyme_\w+)\s+STATIC\b(.*?)\)", re.S)
SOURCE = re.compile(r"[\s(](src/[\w./+-]+\.(?:cpp|cc|cxx))\b")


def library_sources(cmake_file: pathlib.Path) -> dict[str, list[str]]:
    """Map each static library target to the source files it compiles."""
    text = cmake_file.read_text(encoding="utf-8")
    libraries: dict[str, list[str]] = {}
    for name, body in ADD_LIBRARY.findall(text):
        sources = sorted(set(SOURCE.findall(body)))
        if sources:
            libraries[name] = sources
    return libraries


def repo_relative(raw: str) -> str | None:
    """Normalise an export path (absolute or relative) to a repo-relative one."""
    path = pathlib.PurePosixPath(raw)
    if path.is_absolute():
        try:
            path = pathlib.PurePosixPath(
                pathlib.Path(raw).resolve().relative_to(ROOT))
        except ValueError:
            return None
    text = str(path)
    while text.startswith("./"):
        text = text[2:]
    return text


def parse_llvm_export(path: pathlib.Path) -> dict[str, tuple[int, int]]:
    """``llvm-cov export -format=text`` JSON -> {file: (covered, total)}."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    files: dict[str, tuple[int, int]] = {}
    for data in doc.get("data", []):
        for entry in data.get("files", []):
            rel = repo_relative(entry["filename"])
            if rel is None:
                continue
            lines = entry["summary"]["lines"]
            covered, total = int(lines["covered"]), int(lines["count"])
            # llvm-cov merges per-file data across objects; if a duplicate
            # ever appears, keep the largest line set instead of double
            # counting the same lines twice.
            if rel not in files or total > files[rel][1]:
                files[rel] = (covered, total)
    return files


def parse_lcov(path: pathlib.Path) -> dict[str, tuple[int, int]]:
    """lcov ``.info`` -> {file: (covered, total)}."""
    files: dict[str, tuple[int, int]] = {}
    current: str | None = None
    counts: dict[int, int] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("SF:"):
            current = repo_relative(line[3:])
            counts = {}
        elif line.startswith("DA:"):
            fields = line[3:].split(",")
            if len(fields) >= 2 and current is not None:
                try:
                    counts[int(fields[0])] = int(fields[1])
                except ValueError:
                    continue
        elif line.startswith("end_of_record"):
            if current is not None:
                covered = sum(1 for count in counts.values() if count > 0)
                files[current] = (covered, len(counts))
            current = None
    return files


def load_export(path: pathlib.Path) -> dict[str, tuple[int, int]]:
    head = path.read_text(encoding="utf-8", errors="replace").lstrip()[:1]
    if head == "{":
        return parse_llvm_export(path)
    return parse_lcov(path)


def ensure_export(build_dir: pathlib.Path,
                  refresh: bool = False) -> pathlib.Path:
    candidates = (build_dir / "coverage-export.json",
                  build_dir / "coverage.info")
    if not refresh:
        for candidate in candidates:
            if candidate.is_file():
                return candidate
    if not EXPORTER.is_file():
        raise SystemExit(f"coverage export missing and {EXPORTER} not found")
    for candidate in candidates:
        candidate.unlink(missing_ok=True)
    result = subprocess.run(
        ["bash", str(EXPORTER), "--build-dir", str(build_dir),
         "--out-dir", str(build_dir)],
        capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit("coverage export failed")
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        raise SystemExit("coverage export produced no path")
    return pathlib.Path(lines[-1])


def load_thresholds(path: pathlib.Path) -> dict[str, dict]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise SystemExit(f"{path}: {error}") from error
    except json.JSONDecodeError as error:
        raise SystemExit(f"{path}: invalid JSON: {error}") from error
    libraries = doc.get("libraries")
    if not isinstance(libraries, dict) or not libraries:
        raise SystemExit(f"{path}: missing non-empty 'libraries' object")
    thresholds: dict[str, dict] = {}
    for name, entry in libraries.items():
        if isinstance(entry, dict):
            value = entry.get("min_line")
        else:
            value = entry
        if not isinstance(value, (int, float)):
            raise SystemExit(f"{path}: {name}: invalid min_line {value!r}")
        thresholds[name] = {"min_line": float(value),
                            "measured": entry.get("measured")
                            if isinstance(entry, dict) else None}
    return thresholds


def write_baseline(path: pathlib.Path, results: dict[str, dict]) -> None:
    """Persist the measured baseline with one point of headroom."""
    libraries = {}
    for name, result in sorted(results.items()):
        pct = result["percent"]
        libraries[name] = {
            "measured": round(pct, 2),
            "min_line": max(0, math.floor(pct) - 1),
        }
    doc = {
        "comment": ("Measured with a full ctest run on a "
                    "-DTHYME_ENABLE_COVERAGE=ON tree. Threshold is "
                    "floor(measured) - 1: at least one point of headroom "
                    "below the recorded baseline."),
        "libraries": libraries,
    }
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"baseline written to {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default=str(ROOT / "build-cov"),
                        help="coverage build tree (default: build-cov)")
    parser.add_argument("--export", help="explicit export file")
    parser.add_argument("--refresh", action="store_true",
                        help="regenerate the export from build-dir data")
    parser.add_argument("--thresholds", default=str(DEFAULT_THRESHOLDS))
    parser.add_argument("--write-baseline", metavar="PATH",
                        help="write measured baseline to PATH and exit")
    args = parser.parse_args()

    build_dir = pathlib.Path(args.build_dir).resolve()
    if args.export:
        export_path = pathlib.Path(args.export).resolve()
    else:
        export_path = ensure_export(build_dir, refresh=args.refresh)
    if not export_path.is_file():
        print(f"coverage export not found: {export_path}", file=sys.stderr)
        return 2

    measured_files = load_export(export_path)
    sources_by_library = library_sources(ROOT / "CMakeLists.txt")
    if not sources_by_library:
        print("no thyme static libraries found in CMakeLists.txt",
              file=sys.stderr)
        return 2

    results: dict[str, dict] = {}
    missing: dict[str, list[str]] = {}
    for library, sources in sources_by_library.items():
        covered = total = 0
        absent = []
        for source in sources:
            if source in measured_files:
                file_covered, file_total = measured_files[source]
                covered += file_covered
                total += file_total
            else:
                absent.append(source)
        results[library] = {
            "covered": covered,
            "total": total,
            "percent": (100.0 * covered / total) if total else 0.0,
        }
        if absent:
            missing[library] = absent

    if args.write_baseline:
        write_baseline(pathlib.Path(args.write_baseline), results)
        return 0

    thresholds = load_thresholds(pathlib.Path(args.thresholds))
    errors: list[str] = []
    for library in sources_by_library:
        if library not in thresholds:
            errors.append(f"{library}: no threshold in thresholds file")
    for library in thresholds:
        if library not in sources_by_library:
            errors.append(f"{library}: threshold has no matching CMake target")

    for library, absent in missing.items():
        print(f"warning: {library}: no coverage data for "
              f"{len(absent)} source file(s): {', '.join(absent)}",
              file=sys.stderr)
        # A listed source with no export entry was not linked into any
        # executed binary. Dropping it would silently inflate the library's
        # percentage, so treat it as a gate failure.
        errors.append(f"{library}: no coverage data for "
                      f"{len(absent)} source file(s)")
        if results[library]["total"] == 0:
            errors.append(f"{library}: no measured lines")

    header = (f"{'Library':<20} {'Lines':>8} {'Covered':>8} "
              f"{'Line cov':>10} {'Threshold':>10}  Status")
    print(header)
    print("-" * len(header))
    failed = False
    for library in sorted(results):
        result = results[library]
        entry = thresholds.get(library)
        if entry is None:
            print(f"{library:<20} {result['total']:>8} {result['covered']:>8} "
                  f"{result['percent']:>9.2f}% {'-':>10}  NO THRESHOLD")
            failed = True
            continue
        threshold = entry["min_line"]
        ok = result["total"] > 0 and result["percent"] >= threshold
        failed = failed or not ok
        print(f"{library:<20} {result['total']:>8} {result['covered']:>8} "
              f"{result['percent']:>9.2f}% {threshold:>9.2f}%  "
              f"{'PASS' if ok else 'FAIL'}")

    if errors:
        print("\ncoverage gate errors:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
