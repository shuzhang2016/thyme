#!/usr/bin/env python3
"""M7.4 — performance hard gates derived from nightly baseline history.

Reads baseline/<runner_fingerprint>.json (M7.3). After >=7 green cyclic bench
runs on the same runner, derives p99 thresholds (max × slack) and checks a
bench JSON summary. Functional gate: total_drops == 0 always fails unless
BENCH_SOFT=1 (performance-only downgrade; drops still fail).

Usage:
  python3 tools/bench/baseline_gate.py --baseline-dir baseline --bench-json build/bench_smoke.json
  python3 tools/bench/baseline_gate.py --baseline-dir baseline --check-latest

Env:
  BENCH_SOFT=1   performance violations → GATE_WARN, exit 0 (drops still fail)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import sys
from pathlib import Path
from typing import Any

DEFAULT_MIN_RUNS = 7
DEFAULT_SLACK = 1.25


def runner_fingerprint() -> str:
    parts = [
        platform.machine(),
        platform.processor() or "",
        platform.system(),
        platform.release(),
        str(os.cpu_count() or 0),
        os.environ.get("CI_RUNNER_ID", ""),
        os.environ.get("CI_RUNNER_TAGS", ""),
    ]
    digest = hashlib.sha256("|".join(parts).encode()).hexdigest()
    return digest[:16]


def is_green_bench(entry: dict[str, Any]) -> bool:
    if entry.get("status") == "skipped":
        return False
    if entry.get("exit_code", 1) != 0:
        return False
    summary = entry.get("summary")
    if not summary or not isinstance(summary, dict):
        return False
    if "parse_error" in summary:
        return False
    return summary.get("total_drops", 1) == 0


def is_green_run(run: dict[str, Any]) -> bool:
    cyclic = run.get("bench", {}).get("cyclic", {})
    return is_green_bench(cyclic)


def all_runs(doc: dict[str, Any]) -> list[dict[str, Any]]:
    runs: list[dict[str, Any]] = []
    if "latest" in doc:
        runs.append(doc["latest"])
    runs.extend(doc.get("history", []))
    return runs


def green_runs(doc: dict[str, Any]) -> list[dict[str, Any]]:
    return [r for r in all_runs(doc) if is_green_run(r)]


def extract_cyclic_summary(run: dict[str, Any]) -> dict[str, Any] | None:
    cyclic = run.get("bench", {}).get("cyclic", {})
    summary = cyclic.get("summary")
    return summary if isinstance(summary, dict) else None


def derive_thresholds(runs: list[dict[str, Any]], slack: float) -> dict[str, Any]:
    wake: dict[str, int] = {}
    work: dict[str, int] = {}
    pipeline_lat = 0
    for run in runs:
        summary = extract_cyclic_summary(run)
        if not summary:
            continue
        for thread in summary.get("threads", []):
            name = thread.get("name")
            if not name:
                continue
            wake[name] = max(wake.get(name, 0), int(thread.get("p99_wake_err_ns", 0)))
            work[name] = max(work.get(name, 0), int(thread.get("p99_work_ns", 0)))
        pl = summary.get("pipeline", {})
        pipeline_lat = max(pipeline_lat, int(pl.get("p99_lat_ns", 0)))
    return {
        "wake_p99_ns": {k: max(1, int(v * slack)) for k, v in wake.items()},
        "work_p99_ns": {k: max(1, int(v * slack)) for k, v in work.items()},
        "pipeline_p99_lat_ns": max(1, int(pipeline_lat * slack)),
        "source_runs": len(runs),
        "slack": slack,
    }


Violation = tuple[str, int, int]


def check_summary(summary: dict[str, Any], thresholds: dict[str, Any]) -> list[Violation]:
    violations: list[Violation] = []
    drops = int(summary.get("total_drops", 0))
    if drops != 0:
        violations.append(("total_drops", drops, 0))

    for thread in summary.get("threads", []):
        name = thread.get("name", "")
        wake_lim = thresholds["wake_p99_ns"].get(name)
        if wake_lim is not None:
            val = int(thread.get("p99_wake_err_ns", 0))
            if val > wake_lim:
                violations.append((f"{name}.p99_wake_err_ns", val, wake_lim))
        work_lim = thresholds["work_p99_ns"].get(name)
        if work_lim is not None:
            val = int(thread.get("p99_work_ns", 0))
            if val > work_lim:
                violations.append((f"{name}.p99_work_ns", val, work_lim))

    pl = summary.get("pipeline", {})
    lat_lim = thresholds["pipeline_p99_lat_ns"]
    lat_val = int(pl.get("p99_lat_ns", 0))
    if lat_val > lat_lim:
        violations.append(("pipeline.p99_lat_ns", lat_val, lat_lim))
    return violations


def load_baseline(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        with path.open(encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def load_bench_json(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        with path.open(encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"baseline_gate: cannot parse bench json: {exc}", file=sys.stderr)
        return None


def print_gate(label: str, ok: bool) -> None:
    tag = "GATE_OK" if ok else "GATE_WARN"
    print(f"{tag} {label}", file=sys.stderr)


def run_check(
    doc: dict[str, Any],
    summary: dict[str, Any],
    *,
    min_runs: int,
    slack: float,
    bench_soft: bool,
) -> int:
    greens = green_runs(doc)
    n_green = len(greens)
    if n_green < min_runs:
        print(
            f"GATE_SKIP baseline not ready ({n_green}/{min_runs} green cyclic runs)",
            file=sys.stderr,
        )
        return 0

    # Thresholds from prior greens only when checking latest from the same doc.
    threshold_runs = greens[1:] if len(greens) > 1 else greens
    if len(threshold_runs) < min_runs - 1:
        threshold_runs = greens[:-1] if len(greens) >= 2 else greens
    if not threshold_runs:
        threshold_runs = greens

    thresholds = derive_thresholds(threshold_runs, slack)
    violations = check_summary(summary, thresholds)

    print(
        f"=== baseline_gate (green={n_green}, threshold_runs={thresholds['source_runs']}, "
        f"slack={slack}, soft={bench_soft}) ===",
        file=sys.stderr,
    )
    for key, val, lim in violations:
        is_drops = key == "total_drops"
        ok = False
        label = f"{key}={val} <= {lim}" if not is_drops else f"{key}={val} == 0"
        print_gate(label, ok)

    if not violations:
        print_gate("all performance + functional gates", True)
        print("=== end baseline_gate ===", file=sys.stderr)
        return 0

    perf_violations = [v for v in violations if v[0] != "total_drops"]
    drop_violations = [v for v in violations if v[0] == "total_drops"]

    print("=== end baseline_gate ===", file=sys.stderr)

    if drop_violations:
        print("FAIL: total_drops > 0", file=sys.stderr)
        return 1

    if perf_violations and not bench_soft:
        print(f"FAIL: {len(perf_violations)} performance gate(s) exceeded", file=sys.stderr)
        return 1

    if perf_violations and bench_soft:
        print(
            f"BENCH_SOFT: {len(perf_violations)} performance gate(s) downgraded to WARN",
            file=sys.stderr,
        )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="M7.4 baseline-derived performance gates")
    parser.add_argument("--baseline-dir", default="baseline", help="baseline JSON directory")
    parser.add_argument("--baseline", help="explicit baseline JSON path")
    parser.add_argument("--bench-json", help="bench summary JSON to check")
    parser.add_argument(
        "--check-latest",
        action="store_true",
        help="check latest run in baseline file (uses its cyclic summary)",
    )
    parser.add_argument("--min-runs", type=int, default=DEFAULT_MIN_RUNS)
    parser.add_argument("--slack", type=float, default=DEFAULT_SLACK)
    args = parser.parse_args()

    bench_soft = os.environ.get("BENCH_SOFT") == "1"

    if args.baseline:
        baseline_path = Path(args.baseline)
    else:
        fp = runner_fingerprint()
        baseline_path = Path(args.baseline_dir) / f"{fp}.json"

    doc = load_baseline(baseline_path)
    if doc is None:
        print(f"GATE_SKIP no baseline at {baseline_path}", file=sys.stderr)
        return 0

    if args.check_latest:
        latest = doc.get("latest")
        if not latest:
            print("GATE_SKIP baseline has no latest run", file=sys.stderr)
            return 0
        summary = extract_cyclic_summary(latest)
        if not summary:
            print("GATE_SKIP latest run has no cyclic bench summary", file=sys.stderr)
            return 0
    elif args.bench_json:
        summary = load_bench_json(Path(args.bench_json))
        if summary is None:
            return 2
    else:
        parser.error("one of --bench-json or --check-latest is required")

    return run_check(
        doc,
        summary,
        min_runs=args.min_runs,
        slack=args.slack,
        bench_soft=bench_soft,
    )


if __name__ == "__main__":
    sys.exit(main())
