#!/usr/bin/env python3
"""CI-06: generate the acceptance table from a finished CI run or a local build.

Collects the evidence the roadmap asks for and renders it as Markdown plus a
machine-readable JSON document:

* ctest totals (passed/failed/skipped) and per-label counts -- from a JUnit
  report or ``Testing/Temporary/LastTest.log``, joined with the labels in
  ``ctest --show-only=json-v1``;
* per-library line coverage versus ``tools/ci/coverage_thresholds.json`` --
  from an ``llvm-cov export`` JSON or an lcov ``.info`` file;
* the core bench versus ``tools/ci/bench_baseline.json`` for its runner tag;
* gate statuses for ctest/coverage/bench, plus any extra gates supplied with
  ``--gate name=status`` or ``THYME_ACCEPTANCE_GATES``.

Every input is optional: a missing artifact is reported as unavailable instead
of failing the job, so the report can be published even when a nightly job
failed before uploading its numbers. ``--strict`` turns any failed or missing
evidence into a non-zero exit for local use.

Local usage:
  python3 tools/ci/gen_acceptance.py --build-dir build --out /tmp/acceptance.md
"""
from __future__ import annotations

import argparse
import datetime
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[2]
CI_DIR = pathlib.Path(__file__).resolve().parent
DEFAULT_THRESHOLDS = ROOT / "tools" / "ci" / "coverage_thresholds.json"
DEFAULT_BENCH_BASELINE = ROOT / "tools" / "ci" / "bench_baseline.json"


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)  # type: ignore[union-attr]
    return module


check_coverage = load_module("check_coverage", CI_DIR / "check_coverage.py")
check_bench = load_module("check_bench", CI_DIR / "check_bench.py")


def git_sha() -> str:
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, cwd=ROOT)
        return out.stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def resolve_explicit(value: str | None) -> pathlib.Path | None:
    if not value:
        return None
    path = pathlib.Path(value)
    if not path.is_absolute():
        path = (pathlib.Path.cwd() / path).resolve()
    return path if path.is_file() else None


def find_first(paths: list[pathlib.Path]) -> pathlib.Path | None:
    for path in paths:
        if path.is_file():
            return path
    return None


# ----------------------------------------------------------------- tests
def parse_junit(path: pathlib.Path) -> dict[str, str]:
    root = ET.parse(path).getroot()
    results: dict[str, str] = {}
    for case in root.iter("testcase"):
        name = case.get("name") or ""
        if not name:
            continue
        failure = case.find("failure")
        error = case.find("error")
        skipped = case.find("skipped")
        status = (case.get("status") or "").lower()
        if failure is not None or error is not None or status == "fail":
            results[name] = "failed"
        elif skipped is not None or status in ("disabled", "notrun", "skipped"):
            results[name] = "skipped"
        else:
            results[name] = "passed"
    return results


def parse_lasttest(path: pathlib.Path) -> dict[str, str]:
    """LastTest.log fallback. ctest does not mark disabled tests here, so a
    disabled case is indistinguishable from a pass; JUnit is preferred."""
    results: dict[str, str] = {}
    current: str | None = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("Test Passed.") and current is not None:
            results[current] = "passed"
        elif line.startswith("Test Failed.") and current is not None:
            results[current] = "failed"
        elif line.startswith("Test Not Run.") and current is not None:
            results[current] = "skipped"
        else:
            marker = " Testing: "
            if marker in line:
                current = line.split(marker, 1)[1].strip()
    return results


def ctest_metadata(build_dir: pathlib.Path | None,
                   explicit: pathlib.Path | None) -> dict | None:
    """{test_name: {"labels": [...], "disabled": bool}} from ctest JSON."""
    doc = None
    if explicit is not None:
        try:
            doc = json.loads(explicit.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            doc = None
    elif build_dir is not None and build_dir.is_dir():
        try:
            out = subprocess.run(
                ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
                capture_output=True, text=True, timeout=120)
            doc = json.loads(out.stdout) if out.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
            doc = None
    if not doc:
        return None
    metadata: dict[str, dict] = {}
    for test in doc.get("tests", []):
        labels: list[str] = []
        disabled = False
        for prop in test.get("properties", []):
            if prop.get("name") == "LABELS":
                labels = list(prop.get("value") or [])
            elif prop.get("name") == "DISABLED":
                disabled = True
        metadata[test.get("name", "")] = {"labels": labels,
                                          "disabled": disabled}
    return metadata


def summarise_tests(results: dict[str, str],
                    metadata: dict | None) -> dict:
    results = dict(results)
    if metadata:
        # A test disabled in the current tree did not run; LastTest.log even
        # records it as "Test Passed." (ctest logs the skip as a pass), so
        # trust the configure-time metadata over the result file.
        for name, meta in metadata.items():
            if meta.get("disabled"):
                results[name] = "skipped"

    counts = {"passed": 0, "failed": 0, "skipped": 0}
    for status in results.values():
        counts[status] = counts.get(status, 0) + 1

    labels: dict[str, dict] = {}
    if metadata is not None:
        for name, status in results.items():
            meta = metadata.get(name)
            names = (meta["labels"] if meta and meta["labels"]
                     else ["(unlabelled)"])
            for label in names:
                row = labels.setdefault(label, {"total": 0, "passed": 0,
                                                "failed": 0, "skipped": 0})
                row["total"] += 1
                row[status] = row.get(status, 0) + 1
    return {
        "total": len(results),
        "passed": counts["passed"],
        "failed": counts["failed"],
        "skipped": counts["skipped"],
        "labels": labels,
        "failures": sorted(name for name, status in results.items()
                           if status == "failed"),
    }


def collect_tests(args, build_dir: pathlib.Path | None) -> tuple[dict, str]:
    junit = resolve_explicit(args.junit)
    if junit is None and build_dir is not None:
        junit = find_first([build_dir / "ctest-junit.xml",
                            build_dir / "Testing" / "Junit.xml"])
    lasttest = resolve_explicit(args.lasttest)
    if lasttest is None and build_dir is not None:
        lasttest = build_dir / "Testing" / "Temporary" / "LastTest.log"
        lasttest = lasttest if lasttest.is_file() else None

    metadata = ctest_metadata(build_dir, resolve_explicit(args.ctest_json))
    if junit is not None:
        try:
            results, source = parse_junit(junit), str(junit)
        except ET.ParseError:
            results = {}
            source = f"{junit} (unreadable)"
    elif lasttest is not None:
        results, source = parse_lasttest(lasttest), str(lasttest)
    else:
        results, source = {}, "unavailable"
    return summarise_tests(results, metadata), source


# -------------------------------------------------------------- coverage
def collect_coverage(args, build_dir: pathlib.Path | None) -> dict:
    candidates: list[pathlib.Path] = []
    for value in args.coverage_json:
        explicit = resolve_explicit(value)
        if explicit is not None:
            candidates.append(explicit)
    if build_dir is not None:
        candidates += [build_dir / "coverage-export.json",
                       build_dir / "coverage.info",
                       build_dir / "coverage-report" / "coverage-export.json",
                       build_dir / "coverage-report" / "coverage.info"]
    candidates += [pathlib.Path("coverage-report/coverage-export.json"),
                   pathlib.Path("coverage-report/coverage.info")]
    export = find_first([p for p in candidates if p is not None])
    if export is None:
        return {"status": "unavailable", "source": None, "libraries": []}

    try:
        measured = check_coverage.load_export(export)
        thresholds = check_coverage.load_thresholds(
            pathlib.Path(args.coverage_thresholds))
        sources = check_coverage.library_sources(ROOT / "CMakeLists.txt")
    except SystemExit as error:
        return {"status": "unavailable", "source": str(export),
                "libraries": [], "note": str(error)}

    libraries = []
    failed = False
    for library, library_sources in sources.items():
        covered = total = 0
        absent = 0
        for source in library_sources:
            if source in measured:
                covered += measured[source][0]
                total += measured[source][1]
            else:
                absent += 1
        percent = 100.0 * covered / total if total else 0.0
        entry = thresholds.get(library)
        threshold = entry["min_line"] if entry else None
        ok = (total > 0 and absent == 0 and threshold is not None
              and percent >= threshold)
        failed = failed or not ok
        libraries.append({
            "library": library, "covered": covered, "total": total,
            "percent": percent, "threshold": threshold,
            "missing_sources": absent,
            "measured": entry.get("measured") if entry else None,
            "status": "pass" if ok else "fail",
        })
    for library in thresholds:
        if library not in sources:
            failed = True
            libraries.append({
                "library": library, "covered": 0, "total": 0, "percent": 0.0,
                "threshold": thresholds[library]["min_line"],
                "missing_sources": 0, "measured": None, "status": "fail",
                "note": "threshold without a matching CMake target"})
    return {"status": "fail" if failed else "pass", "source": str(export),
            "libraries": libraries}


# ----------------------------------------------------------------- bench
def collect_bench(args, build_dir: pathlib.Path | None) -> dict:
    candidates = []
    explicit = resolve_explicit(args.bench_json)
    if explicit is not None:
        candidates.append(explicit)
    if build_dir is not None:
        candidates.append(build_dir / "bench_core.json")
    candidates += [pathlib.Path("bench-report/bench_core.json"),
                   pathlib.Path("bench_core.json")]
    run_path = find_first([p for p in candidates if p is not None])
    if run_path is None:
        return {"status": "unavailable", "source": None, "rows": []}
    try:
        run = check_bench.load_json(run_path)
        check_bench.validate_run(run, run_path)
    except SystemExit as error:
        return {"status": "unavailable", "source": str(run_path), "rows": [],
                "note": str(error)}

    baseline_path = pathlib.Path(args.bench_baseline)
    baseline = check_bench.load_json(baseline_path) if baseline_path.is_file() else {}
    tolerance = check_bench.tolerance_for(baseline, args.bench_tolerance)
    result = check_bench.compare(run, baseline, tolerance)
    result["source"] = str(run_path)
    result["bench_git_sha"] = run.get("git_sha")
    result["build_type"] = run.get("build_type")
    result["toolchain"] = run.get("toolchain")
    return result


# ------------------------------------------------------------------ gates
def parse_gate_specs(values: list[str]) -> list[dict]:
    gates = []
    normalized = {"pass": "pass", "passed": "pass", "success": "pass",
                  "ok": "pass", "fail": "fail", "failed": "fail",
                  "failure": "fail", "skip": "skip", "skipped": "skip"}
    for value in values:
        for token in value.replace(";", ",").split(","):
            token = token.strip()
            if not token or "=" not in token:
                continue
            name, status = token.split("=", 1)
            status = normalized.get(status.strip().lower(), status.strip())
            gates.append({"name": name.strip(), "status": status})
    return gates


def collect_gates(args, tests: dict, coverage: dict, bench: dict) -> list[dict]:
    def tests_status() -> str:
        if tests.get("total", 0) == 0:
            return "unavailable"
        return "fail" if tests.get("failed", 0) else "pass"

    gates = [
        {"name": "ctest", "status": tests_status()},
        {"name": "coverage", "status": coverage.get("status", "unavailable")},
        {"name": "bench", "status": bench.get("status", "unavailable")},
    ]
    gates += parse_gate_specs(args.gate)
    env_gates = os.environ.get("THYME_ACCEPTANCE_GATES")
    if env_gates:
        gates += parse_gate_specs([env_gates])
    return gates


# ------------------------------------------------------------- rendering
def render_markdown(doc: dict) -> str:
    lines: list[str] = []
    lines.append("# Acceptance report")
    lines.append("")
    lines.append(f"- Generated: {doc['generated_at']}")
    lines.append(f"- Commit: `{doc['git_sha']}`")
    if doc.get("pipeline"):
        pipeline = doc["pipeline"]
        parts = [f"{key}={value}" for key, value in pipeline.items() if value]
        if parts:
            lines.append(f"- Pipeline: {', '.join(parts)}")
    bench_build = doc.get("bench") or {}
    if bench_build.get("build_type") or bench_build.get("toolchain"):
        lines.append(f"- Benchmark build: {bench_build.get('build_type', '?')} "
                     f"({bench_build.get('toolchain', '?')})")
    lines.append("")

    tests = doc.get("tests", {})
    coverage = doc.get("coverage", {})
    bench = doc.get("bench", {})
    lines.append("## Summary")
    lines.append("")
    lines.append("| Area | Status | Evidence |")
    lines.append("|------|--------|----------|")
    lines.append(f"| Tests | {tests.get('passed', 0)} passed / "
                 f"{tests.get('total', 0)} total | {doc['sources'].get('tests')} |")
    lines.append(f"| Coverage | {coverage.get('status', 'unavailable')} | "
                 f"{coverage.get('source') or 'no export'} |")
    bench_evidence = bench.get("source") or "no bench run"
    if bench.get("runner_tag"):
        bench_evidence += f" (runner {bench['runner_tag']})"
    lines.append(f"| Bench | {bench.get('status', 'unavailable')} | "
                 f"{bench_evidence} |")
    extra = [g for g in doc["gates"]
             if g["name"] not in ("ctest", "coverage", "bench")]
    if extra:
        lines.append("| Other gates | " +
                     ", ".join(f"{g['name']}={g['status']}" for g in extra) +
                     " | env/--gate |")
    lines.append("")

    lines.append("## Tests")
    lines.append("")
    lines.append(f"Total {tests.get('total', 0)} · passed "
                 f"{tests.get('passed', 0)} · failed {tests.get('failed', 0)} · "
                 f"skipped {tests.get('skipped', 0)}")
    if tests.get("failures"):
        lines.append("")
        lines.append("Failed: " + ", ".join(f"`{name}`"
                                            for name in tests["failures"]))
    labels = tests.get("labels") or {}
    if labels:
        lines.append("")
        lines.append("| Label | Tests | Passed | Failed | Skipped |")
        lines.append("|-------|-------|--------|--------|---------|")
        for label in sorted(labels):
            row = labels[label]
            lines.append(f"| `{label}` | {row['total']} | "
                         f"{row.get('passed', 0)} | {row.get('failed', 0)} | "
                         f"{row.get('skipped', 0)} |")
    else:
        lines.append("")
        lines.append("Per-label counts unavailable (no ctest metadata).")
    lines.append("")

    lines.append("## Coverage (per library)")
    lines.append("")
    if coverage.get("libraries"):
        lines.append("| Library | Lines | Covered | Line cov | Threshold | Status |")
        lines.append("|---------|-------|---------|----------|-----------|--------|")
        for row in coverage["libraries"]:
            threshold = ("-" if row["threshold"] is None
                         else f"{row['threshold']:.1f}%")
            lines.append(f"| `{row['library']}` | {row['total']} | "
                         f"{row['covered']} | {row['percent']:.2f}% | "
                         f"{threshold} | {row['status'].upper()} |")
    else:
        lines.append("Coverage unavailable (no llvm-cov/lcov export found).")
    lines.append("")

    lines.append("## Bench vs baseline")
    lines.append("")
    if bench.get("rows"):
        lines.append(f"Runner `{bench.get('runner_tag', '?')}`, tolerance "
                     f"+{bench.get('tolerance_pct', 0):.1f}% ns/op. Baseline "
                     "is per-runner, not a target-board budget.")
        lines.append("")
        lines.append("| Metric | Baseline ns/op | Current ns/op | Delta | GB/s | Status |")
        lines.append("|--------|----------------|---------------|-------|------|--------|")
        for row in bench["rows"]:
            base = ("-" if row["baseline_ns_per_op"] is None
                    else f"{row['baseline_ns_per_op']:.3f}")
            current = ("-" if row["run_ns_per_op"] is None
                       else f"{row['run_ns_per_op']:.3f}")
            delta = ("-" if row["delta_pct"] is None
                     else f"{row['delta_pct']:+.1f}%")
            gbps = ("-" if row["gbps"] is None else f"{float(row['gbps']):.2f}")
            lines.append(f"| `{row['metric']}` | {base} | {current} | "
                         f"{delta} | {gbps} | {row['status'].upper()} |")
        if bench.get("note"):
            lines.append("")
            lines.append(f"Note: {bench['note']}")
    else:
        lines.append("Bench unavailable (no `bench_core.json` found).")
    lines.append("")

    lines.append("## Gates")
    lines.append("")
    lines.append("| Gate | Status |")
    lines.append("|------|--------|")
    for gate in doc["gates"]:
        lines.append(f"| {gate['name']} | {gate['status'].upper()} |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default=str(ROOT / "build"),
                        help="build tree with Testing/ and bench_core.json")
    parser.add_argument("--out", default=str(ROOT / "acceptance.md"),
                        help="Markdown output path")
    parser.add_argument("--json-out", help="JSON output path "
                        "(default: <out dir>/acceptance.json)")
    parser.add_argument("--junit", help="ctest JUnit XML")
    parser.add_argument("--lasttest", help="ctest LastTest.log")
    parser.add_argument("--ctest-json", help="ctest --show-only=json-v1 output")
    parser.add_argument("--coverage-json", action="append", default=[],
                        help="llvm-cov export JSON or lcov .info (repeatable)")
    parser.add_argument("--coverage-thresholds", default=str(DEFAULT_THRESHOLDS))
    parser.add_argument("--bench-json", help="bench_core --json run")
    parser.add_argument("--bench-baseline", default=str(DEFAULT_BENCH_BASELINE))
    parser.add_argument("--bench-tolerance", type=float,
                        help="override the baseline's ns/op tolerance")
    parser.add_argument("--gate", action="append", default=[],
                        help="extra gate as name=status (repeatable)")
    parser.add_argument("--git-sha")
    parser.add_argument("--timestamp")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when a gate failed or evidence is missing")
    args = parser.parse_args()

    build_dir = pathlib.Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = (pathlib.Path.cwd() / build_dir).resolve()
    if not build_dir.is_dir():
        build_dir = None

    tests, tests_source = collect_tests(args, build_dir)
    coverage = collect_coverage(args, build_dir)
    bench = collect_bench(args, build_dir)
    gates = collect_gates(args, tests, coverage, bench)

    pipeline = {key: os.environ.get(key) for key in
                ("CI_PIPELINE_ID", "CI_JOB_NAME", "CI_COMMIT_REF_NAME",
                 "CI_PIPELINE_URL")}
    pipeline = {key: value for key, value in pipeline.items() if value}

    doc = {
        "schema": 1,
        "kind": "thyme-acceptance",
        "generated_at": args.timestamp or datetime.datetime.now(
            datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_sha": args.git_sha or os.environ.get("CI_COMMIT_SHORT_SHA")
        or git_sha(),
        "pipeline": pipeline,
        "sources": {
            "tests": tests_source,
            "coverage": coverage.get("source"),
            "bench": bench.get("source"),
        },
        "tests": tests,
        "coverage": coverage,
        "bench": bench,
        "gates": gates,
    }

    out_path = pathlib.Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(render_markdown(doc), encoding="utf-8")
    json_path = (pathlib.Path(args.json_out) if args.json_out
                 else out_path.with_suffix(".json"))
    json_path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"acceptance report written to {out_path} and {json_path}")

    if args.strict:
        bad = [gate["name"] for gate in gates
               if gate["status"] not in ("pass", "skip")]
        if bad:
            print(f"acceptance --strict: failing gates: {', '.join(bad)}",
                  file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
