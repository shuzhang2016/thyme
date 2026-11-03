#!/usr/bin/env python3
"""OBS-10: per-runner gate for eval_chains cpu/io/irq baselines.

Compares a normalized ``thyme-eval-chains-run`` document (produced by
``tools/ci/run_eval_chains.py``) against
``tools/ci/eval_chains_baseline.json``. The conventions mirror
``tools/ci/check_bench.py``: baselines are keyed by runner tag, the tolerance
is a percentage, a metric that exists in the baseline but is missing from the
run fails, and a metric with no baseline is reported as NEW and not gated.

Metrics are relative deltas:

  frames_per_sec         higher is better (completed E2E joins per second)
  e2e_p95_ns             lower is better (frame-joined latency p95)

The default tolerance is deliberately loose (100%) because the multi-threaded
chain under synthetic load is bimodal on a shared host: a hop either catches
the next period or the one after. The gate catches order-of-magnitude
regressions and missing/zero metrics; a runner with stable history can lower
``tolerance_pct`` in the baseline file.

A scenario missing from the run, or a run scenario whose status is not ``ok``,
fails the gate. ``broken``/``broken_pct`` are integrity signals reported by the
driver (which enforces its own --max-broken-pct); they are not tolerance-gated.

Exit codes: 0 = pass, or skipped (no baseline / host mismatch without
--require-host-match); 1 = regression, missing scenario/metric or a failed run
scenario; 2 = usage or I/O error.
"""

from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_BASELINE = ROOT / "tools" / "ci" / "eval_chains_baseline.json"
DEFAULT_TOLERANCE_PCT = 100.0
KIND = "thyme-eval-chains-run"


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise SystemExit(f"{path}: {error}") from error
    except json.JSONDecodeError as error:
        raise SystemExit(f"{path}: invalid JSON: {error}") from error


def validate_run(run: dict[str, Any], path: pathlib.Path) -> None:
    if run.get("kind") != KIND:
        raise SystemExit(f"{path}: not a {KIND} document")
    if not run.get("runner_tag"):
        raise SystemExit(f"{path}: missing runner_tag")
    if not isinstance(run.get("scenarios"), dict) or not run["scenarios"]:
        raise SystemExit(f"{path}: missing non-empty 'scenarios' object")
    for name, scenario in run["scenarios"].items():
        if not isinstance(scenario.get("metrics"), dict):
            raise SystemExit(f"{path}: scenario {name!r} missing 'metrics'")


def tolerance_for(baseline: dict[str, Any], override: float | None) -> float:
    if override is not None:
        return float(override)
    value = baseline.get("tolerance_pct", DEFAULT_TOLERANCE_PCT)
    try:
        return float(value)
    except (TypeError, ValueError):
        raise SystemExit(f"invalid tolerance_pct: {value!r}")


HIGHER_IS_BETTER = ("frames", "frames_per_sec")


def better_direction(metric: str) -> str:
    return "higher" if metric in HIGHER_IS_BETTER else "lower"


def host_mismatch(run: dict[str, Any], entry: dict[str, Any]) -> str | None:
    """Return a human-readable mismatch, or None when the hosts agree."""
    base_host = entry.get("host") or {}
    run_host = run.get("host") or {}
    base_machine = base_host.get("machine")
    run_machine = run_host.get("machine")
    if base_machine and run_machine and base_machine != run_machine:
        return f"arch {run_machine!r} != baseline {base_machine!r}"
    base_hostname = base_host.get("hostname")
    run_hostname = run_host.get("hostname")
    if base_hostname and run_hostname and base_hostname != run_hostname:
        return f"hostname {run_hostname!r} != baseline {base_hostname!r}"
    return None


def metric_status(metric: str, baseline_value: float, run_value: float,
                  tolerance: float) -> tuple[str, float | None]:
    direction = better_direction(metric)
    if baseline_value == 0:
        if direction == "higher":
            return ("pass" if run_value >= 0 else "fail"), None
        return ("pass" if run_value <= 0 else "fail"), None
    delta = (run_value / baseline_value - 1.0) * 100.0
    if direction == "higher":
        ok = delta >= -tolerance
    else:
        ok = delta <= tolerance
    return ("pass" if ok else "fail"), delta


def compare(run: dict[str, Any], baseline: dict[str, Any], tolerance: float,
            require_host_match: bool) -> dict[str, Any]:
    tag = run["runner_tag"]
    entry = (baseline.get("runners") or {}).get(tag)
    result: dict[str, Any] = {
        "schema": 1,
        "kind": "thyme-eval-chains-gate",
        "generated_at": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "runner_tag": tag,
        "tolerance_pct": tolerance,
        "baseline_recorded_at": entry.get("recorded_at") if entry else None,
        "baseline_git_sha": entry.get("git_sha") if entry else None,
        "rows": [],
        "status": "no-baseline",
    }
    if entry is None:
        result["note"] = (
            f"no baseline for runner {tag!r}; run --write-baseline on this "
            "runner to record one")
        for name, scenario in run["scenarios"].items():
            for metric, value in scenario["metrics"].items():
                result["rows"].append({
                    "scenario": name, "metric": metric,
                    "baseline": None, "run": value, "delta_pct": None,
                    "status": "new"})
        return result

    mismatch = host_mismatch(run, entry)
    if mismatch is not None:
        result["status"] = "host-mismatch"
        result["note"] = f"host mismatch: {mismatch}"
        if require_host_match:
            result["status"] = "fail"
        return result

    failed = False
    for name, base_scenario in (entry.get("scenarios") or {}).items():
        run_scenario = run["scenarios"].get(name)
        if run_scenario is None:
            result["rows"].append({
                "scenario": name, "metric": "*",
                "baseline": None, "run": None, "delta_pct": None,
                "status": "missing-scenario"})
            failed = True
            continue
        if run_scenario.get("status") != "ok":
            result["rows"].append({
                "scenario": name, "metric": "*",
                "baseline": None, "run": None, "delta_pct": None,
                "status": "failed-run"})
            failed = True
        base_metrics = base_scenario.get("metrics") or {}
        run_metrics = run_scenario.get("metrics") or {}
        for metric, base_value in base_metrics.items():
            if metric not in run_metrics:
                result["rows"].append({
                    "scenario": name, "metric": metric,
                    "baseline": base_value, "run": None, "delta_pct": None,
                    "status": "missing"})
                failed = True
                continue
            run_value = run_metrics[metric]
            status, delta = metric_status(
                metric, float(base_value), float(run_value), tolerance)
            failed = failed or status == "fail"
            result["rows"].append({
                "scenario": name, "metric": metric,
                "baseline": base_value, "run": run_value,
                "delta_pct": delta, "status": status})
        for metric, run_value in run_metrics.items():
            if metric not in base_metrics:
                result["rows"].append({
                    "scenario": name, "metric": metric,
                    "baseline": None, "run": run_value, "delta_pct": None,
                    "status": "new"})

    result["status"] = "fail" if failed else "pass"
    return result


def print_table(result: dict[str, Any]) -> None:
    print(f"eval_chains gate: runner={result['runner_tag']} "
          f"tolerance=+/-{result['tolerance_pct']:.1f}%")
    if result.get("baseline_recorded_at"):
        print(f"baseline: recorded {result['baseline_recorded_at']} "
              f"at {result['baseline_git_sha']}")
    header = (f"{'Scenario':<9} {'Metric':<34} {'Baseline':>14} "
              f"{'Current':>14} {'Delta':>9}  Status")
    print(header)
    print("-" * len(header))
    for row in result["rows"]:
        base = "-" if row["baseline"] is None else str(row["baseline"])
        current = "-" if row["run"] is None else str(row["run"])
        delta = ("-" if row["delta_pct"] is None
                 else f"{row['delta_pct']:+.1f}%")
        print(f"{row['scenario']:<9} {row['metric']:<34} {base:>14} "
              f"{current:>14} {delta:>9}  {row['status'].upper()}")
    if result.get("note"):
        print(f"\nnote: {result['note']}")


def write_baseline(path: pathlib.Path, run: dict[str, Any],
                   baseline: dict[str, Any] | None) -> None:
    for name, scenario in run["scenarios"].items():
        if scenario.get("status") != "ok":
            raise SystemExit(
                f"refusing to record a baseline: scenario {name!r} status "
                f"{scenario.get('status')!r}")
    doc = baseline if baseline is not None else {
        "comment": (
            "OBS-10 eval_chains cpu/io/irq baselines. Keyed by runner tag, "
            "same convention as tools/ci/bench_baseline.json: x86_64-local is "
            "the development machine and is reference-only evidence until the "
            "runner tags a baseline, x86_64-ci is the GitLab runner and "
            "records its own with --write-baseline. Never copy numbers "
            "between runners. tolerance_pct is deliberately wide because the "
            "chain latency is bimodal under synthetic load on a shared host; "
            "lower it per runner once the history is stable."),
        "tolerance_pct": DEFAULT_TOLERANCE_PCT,
        "runners": {},
    }
    runners = doc.setdefault("runners", {})
    scenarios: dict[str, Any] = {}
    for name, scenario in run["scenarios"].items():
        scenarios[name] = {
            "frames": scenario.get("frames"),
            "broken": scenario.get("broken"),
            "broken_pct": scenario.get("broken_pct"),
            "metrics": scenario.get("metrics") or {},
        }
    runners[run["runner_tag"]] = {
        "recorded_at": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_sha": run.get("git_sha"),
        "build_type": run.get("build_type"),
        "host": run.get("host") or {},
        "scenarios": scenarios,
    }
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"baseline written to {path} for runner "
          f"{run['runner_tag']!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run", required=True,
                        help="normalized run JSON from run_eval_chains.py")
    parser.add_argument("--baseline", default=str(DEFAULT_BASELINE),
                        help="baseline file (default: tools/ci/eval_chains_baseline.json)")
    parser.add_argument("--tolerance", type=float,
                        help="allowed regression in percent "
                             f"(default: baseline value, else "
                             f"{DEFAULT_TOLERANCE_PCT:g})")
    parser.add_argument("--write-baseline", action="store_true",
                        help="record this run as the baseline for its runner tag")
    parser.add_argument("--require-baseline", action="store_true",
                        help="fail instead of skipping when the runner tag has "
                             "no recorded baseline")
    parser.add_argument("--require-host-match", action="store_true",
                        help="fail on hostname/arch mismatch instead of skipping")
    parser.add_argument("--json-out", help="write the structured gate result")
    args = parser.parse_args()

    run_path = pathlib.Path(args.run).resolve()
    run = load_json(run_path)
    validate_run(run, run_path)

    baseline_path = pathlib.Path(args.baseline).resolve()
    baseline = None
    if baseline_path.is_file():
        baseline = load_json(baseline_path)

    if args.write_baseline:
        write_baseline(baseline_path, run, baseline)
        return 0

    baseline = baseline or {}
    tolerance = tolerance_for(baseline, args.tolerance)
    result = compare(run, baseline, tolerance, args.require_host_match)
    print_table(result)
    if args.json_out:
        pathlib.Path(args.json_out).write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8")

    sys.stdout.flush()
    if result["status"] == "no-baseline":
        print(f"\nwarning: no baseline for runner {result['runner_tag']!r}; "
              "gate skipped. Record one with --write-baseline on that runner.",
              file=sys.stderr)
        return 1 if args.require_baseline else 0
    if result["status"] == "host-mismatch":
        print(f"\nwarning: {result['note']}; gate skipped. "
              "Record a baseline on this host or pass --require-host-match.",
              file=sys.stderr)
        return 0
    if result["status"] == "fail":
        failed = [f"{row['scenario']}.{row['metric']}" for row in result["rows"]
                  if row["status"] in ("fail", "missing", "missing-scenario",
                                       "failed-run")]
        print(f"\neval_chains gate FAILED: {', '.join(failed)}",
              file=sys.stderr)
        return 1
    print("\neval_chains gate PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
