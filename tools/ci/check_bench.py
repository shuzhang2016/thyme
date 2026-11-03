#!/usr/bin/env python3
"""PERF-06: per-runner bench baseline gate.

Compares fresh bench JSON documents against ``tools/ci/bench_baseline.json``.
Three documents are understood and may be passed together with repeated
``--run`` options:

  thyme-bench-core          bench_core (ns/op + sampled p99/p999)
  thyme-bench-object-pool   bench_object_pool (ns/op + p99/p999)
  thyme-bench-trigger-seal  bench_trigger_seal (p50/p99/p999 distribution;
                            the p50 is the gate's ns/op)

Baselines are keyed by runner tag and carry the recording host. Numbers from
a developer box (``x86_64-local``) are never used as a target-board budget,
and the CI runner (``x86_64-ci``) records its own baseline on its first run.
A run whose host/arch differs from the baseline entry is reported as
``host-mismatch`` and skipped, mirroring ``tools/ci/check_eval_chains.py``
(OBS-10); pass ``--require-host-match`` to turn that into a failure.

Tolerance policy (documented in docs/guides/bench-baselines.md):

  ns/op  +25%   (steady-state cost)
  p99    +50%   (tail noise on a shared host)
  p999   +100%  (nearest-rank tail; wider because it is a few samples)

A metric present in the baseline but missing from the run fails. A newly
added metric (or a newly emitted percentile) is reported as NEW and is not
gated until the baseline is refreshed.

Exit codes: 0 = gate passed, or skipped (no baseline / host mismatch without
the matching --require-* flag); 1 = regression/missing metric/failed
correctness check; 2 = usage or I/O error.
"""
from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_BASELINE = ROOT / "tools" / "ci" / "bench_baseline.json"
DEFAULT_TOLERANCE_PCT = 25.0
DEFAULT_P99_TOLERANCE_PCT = 50.0
DEFAULT_P999_TOLERANCE_PCT = 100.0

CORE_KIND = "thyme-bench-core"
OBJECT_POOL_KIND = "thyme-bench-object-pool"
TRIGGER_SEAL_KIND = "thyme-bench-trigger-seal"
BENCH_KINDS = (CORE_KIND, OBJECT_POOL_KIND, TRIGGER_SEAL_KIND)
SEAL_METRIC = "trigger_to_seal"

# Row-level statistics. "new" is informational and never fails the gate.
FAILING_STATUSES = ("fail", "missing")


def load_json(path: pathlib.Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise SystemExit(f"{path}: {error}") from error
    except json.JSONDecodeError as error:
        raise SystemExit(f"{path}: invalid JSON: {error}") from error


def _number(value: Any, what: str, path: pathlib.Path) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError):
        raise SystemExit(f"{path}: {what} is not a number: {value!r}") from None
    if number < 0:
        raise SystemExit(f"{path}: {what} must be non-negative: {number}")
    return number


def _optional_number(value: Any, what: str,
                     path: pathlib.Path) -> float | None:
    if value is None:
        return None
    return _number(value, what, path)


def _host_of(run: dict) -> dict:
    host = run.get("host") or {}
    return {
        "hostname": host.get("hostname") or run.get("hostname"),
        "machine": host.get("machine"),
    }


def _normalize_core_metric(item: dict, path: pathlib.Path) -> dict:
    name = item.get("name")
    if not isinstance(name, str) or not name:
        raise SystemExit(f"{path}: benchmark without a name")
    bytes_per_op = item.get("bytes_per_op", 0)
    try:
        bytes_per_op = int(bytes_per_op or 0)
    except (TypeError, ValueError):
        raise SystemExit(
            f"{path}: {name}.bytes_per_op is not an int") from None
    return {
        "name": name,
        "ns_per_op": _number(item.get("ns_per_op"), f"{name}.ns_per_op", path),
        "p99_ns_per_op": _optional_number(
            item.get("p99_ns_per_op"), f"{name}.p99_ns_per_op", path),
        "p999_ns_per_op": _optional_number(
            item.get("p999_ns_per_op"), f"{name}.p999_ns_per_op", path),
        "bytes_per_op": bytes_per_op,
        "iterations": int(item.get("iterations", 0) or 0),
        "gbps": item.get("gbps"),
    }


def normalize_run(run: dict, path: pathlib.Path) -> dict:
    """Validate one bench document and flatten it into metric rows."""
    kind = run.get("kind")
    if kind not in BENCH_KINDS:
        raise SystemExit(f"{path}: not one of {', '.join(BENCH_KINDS)}")
    tag = run.get("runner_tag")
    if not tag:
        raise SystemExit(f"{path}: missing runner_tag")

    metrics: dict[str, dict] = {}
    if kind == TRIGGER_SEAL_KIND:
        distribution = run.get("trigger_to_seal_ns")
        if not isinstance(distribution, dict):
            raise SystemExit(f"{path}: missing trigger_to_seal_ns object")
        metrics[SEAL_METRIC] = {
            "name": SEAL_METRIC,
            "ns_per_op": _number(distribution.get("p50"),
                                 "trigger_to_seal_ns.p50", path),
            "p99_ns_per_op": _optional_number(
                distribution.get("p99"), "trigger_to_seal_ns.p99", path),
            "p999_ns_per_op": _optional_number(
                distribution.get("p999"), "trigger_to_seal_ns.p999", path),
            "bytes_per_op": 0,
            "iterations": int(distribution.get("n", 0) or 0),
            "gbps": None,
        }
    else:
        items = run.get("benchmarks")
        if not isinstance(items, list) or not items:
            raise SystemExit(f"{path}: missing non-empty 'benchmarks' list")
        for item in items:
            metric = _normalize_core_metric(item, path)
            if metric["name"] in metrics:
                raise SystemExit(
                    f"{path}: duplicate benchmark {metric['name']!r}")
            metrics[metric["name"]] = metric

    return {
        "path": path,
        "kind": kind,
        "runner_tag": tag,
        "host": _host_of(run),
        "ok": bool(run.get("ok", True)),
        "metrics": metrics,
    }


def validate_run(run: dict, path: pathlib.Path) -> None:
    """Backwards-compatible single-document validator."""
    normalize_run(run, path)


def tolerance_for(baseline: dict, override: float | None) -> float:
    """ns/op tolerance (kept for callers that only need the primary one)."""
    if override is not None:
        return float(override)
    value = baseline.get("tolerance_ns_per_op_pct", DEFAULT_TOLERANCE_PCT)
    try:
        return float(value)
    except (TypeError, ValueError):
        raise SystemExit(f"invalid tolerance_ns_per_op_pct: {value!r}")


def tolerance_set(baseline: dict, ns: float | None, p99: float | None,
                  p999: float | None) -> dict[str, float]:
    def pick(override: float | None, key: str, default: float) -> float:
        if override is not None:
            return float(override)
        value = baseline.get(key, default)
        try:
            return float(value)
        except (TypeError, ValueError):
            raise SystemExit(f"invalid {key}: {value!r}")
    return {
        "ns_per_op": pick(ns, "tolerance_ns_per_op_pct", DEFAULT_TOLERANCE_PCT),
        "p99": pick(p99, "tolerance_p99_pct", DEFAULT_P99_TOLERANCE_PCT),
        "p999": pick(p999, "tolerance_p999_pct", DEFAULT_P999_TOLERANCE_PCT),
    }


def derived_gbps(metric: dict) -> float | None:
    """bytes/ns is GB/s; recompute it so the table always matches ns/op."""
    bytes_per_op = float(metric.get("bytes_per_op") or 0)
    ns = float(metric.get("ns_per_op") or 0)
    if bytes_per_op > 0 and ns > 0:
        return bytes_per_op / ns
    return metric.get("gbps")


def host_mismatch(run: dict, entry: dict) -> str | None:
    """Return a human-readable mismatch, or None when the hosts agree."""
    base_host = entry.get("host") or {}
    if not base_host and entry.get("hostname"):
        base_host = {"hostname": entry.get("hostname")}
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


def _delta(run_value: float, base_value: float) -> float:
    if base_value == 0:
        return 0.0 if run_value == 0 else float("inf")
    return (run_value / base_value - 1.0) * 100.0


def _stat_status(baseline_value: float | None, run_value: float | None,
                 tolerance: float) -> tuple[str | None, float | None]:
    if baseline_value is None:
        return ("new" if run_value is not None else None), None
    if run_value is None:
        return "missing", None
    delta = _delta(run_value, baseline_value)
    return ("pass" if delta <= tolerance else "fail"), delta


def _worst(statuses: list[str | None]) -> str:
    for status in ("fail", "missing"):
        if status in statuses:
            return status
    return "pass"


def metric_kind(name: str, recorded: dict) -> str:
    """Which bench document a baseline metric belongs to.

    New baselines store the kind explicitly; older entries are inferred from
    the metric name so a baseline refresh is not required just to add the
    field.
    """
    kind = recorded.get("bench")
    if kind in BENCH_KINDS:
        return kind
    if name == SEAL_METRIC:
        return TRIGGER_SEAL_KIND
    if name.startswith("object_pool_"):
        return OBJECT_POOL_KIND
    return CORE_KIND


def _compare_metrics(docs: list[dict], entry: dict,
                     tolerances: dict[str, float]) -> tuple[list[dict], bool]:
    run_metrics: dict[str, dict] = {}
    for doc in docs:
        for name, metric in doc["metrics"].items():
            if name in run_metrics:
                raise SystemExit(
                    f"metric {name!r} appears in more than one --run document")
            run_metrics[name] = metric

    baseline_metrics = entry.get("benchmarks") or {}
    selected_kinds = {doc["kind"] for doc in docs}
    rows: list[dict] = []
    failed = False

    for name, recorded in baseline_metrics.items():
        # Comparing only bench_core must not report the object-pool or
        # trigger-seal rows as missing; each document gates only its own
        # metrics.
        if metric_kind(name, recorded) not in selected_kinds:
            continue
        base_ns = _number(recorded.get("ns_per_op"), f"{name}.ns_per_op",
                          pathlib.Path("<baseline>"))
        base_p99 = _optional_number(recorded.get("p99_ns_per_op"),
                                    f"{name}.p99_ns_per_op",
                                    pathlib.Path("<baseline>"))
        base_p999 = _optional_number(recorded.get("p999_ns_per_op"),
                                     f"{name}.p999_ns_per_op",
                                     pathlib.Path("<baseline>"))
        current = run_metrics.get(name)
        if current is None:
            rows.append({
                "metric": name,
                "baseline_ns_per_op": base_ns, "run_ns_per_op": None,
                "delta_pct": None, "gbps": None,
                "baseline_p99_ns_per_op": base_p99,
                "run_p99_ns_per_op": None, "delta_p99_pct": None,
                "p99_status": "missing",
                "baseline_p999_ns_per_op": base_p999,
                "run_p999_ns_per_op": None, "delta_p999_pct": None,
                "p999_status": "missing",
                "status": "missing",
            })
            failed = True
            continue

        run_ns = float(current["ns_per_op"])
        delta = _delta(run_ns, base_ns)
        ns_status = "pass" if delta <= tolerances["ns_per_op"] else "fail"
        p99_status, delta_p99 = _stat_status(
            base_p99, current.get("p99_ns_per_op"), tolerances["p99"])
        p999_status, delta_p999 = _stat_status(
            base_p999, current.get("p999_ns_per_op"), tolerances["p999"])
        status = _worst([ns_status, p99_status, p999_status])
        failed = failed or status in FAILING_STATUSES
        rows.append({
            "metric": name,
            "baseline_ns_per_op": base_ns, "run_ns_per_op": run_ns,
            "delta_pct": delta, "gbps": derived_gbps(current),
            "baseline_p99_ns_per_op": base_p99,
            "run_p99_ns_per_op": current.get("p99_ns_per_op"),
            "delta_p99_pct": delta_p99, "p99_status": p99_status,
            "baseline_p999_ns_per_op": base_p999,
            "run_p999_ns_per_op": current.get("p999_ns_per_op"),
            "delta_p999_pct": delta_p999, "p999_status": p999_status,
            "status": status,
        })

    for name, current in run_metrics.items():
        if name in baseline_metrics:
            continue
        rows.append({
            "metric": name,
            "baseline_ns_per_op": None, "run_ns_per_op": current["ns_per_op"],
            "delta_pct": None, "gbps": derived_gbps(current),
            "baseline_p99_ns_per_op": None,
            "run_p99_ns_per_op": current.get("p99_ns_per_op"),
            "delta_p99_pct": None, "p99_status": "new",
            "baseline_p999_ns_per_op": None,
            "run_p999_ns_per_op": current.get("p999_ns_per_op"),
            "delta_p999_pct": None, "p999_status": "new",
            "status": "new",
        })
    return rows, failed


def compare_documents(docs: list[dict], baseline: dict,
                      tolerances: dict[str, float],
                      require_host_match: bool = False) -> dict:
    """Build the gate result for one or more normalized run documents."""
    if not docs:
        raise SystemExit("no bench documents to compare")
    tag = docs[0]["runner_tag"]
    host = docs[0]["host"]
    for doc in docs[1:]:
        if doc["runner_tag"] != tag:
            raise SystemExit(
                f"{doc['path']}: runner_tag {doc['runner_tag']!r} != "
                f"{tag!r} from {docs[0]['path']}")
        if doc["host"] != host:
            raise SystemExit(
                f"{doc['path']}: host {doc['host']!r} != {host!r} from "
                f"{docs[0]['path']}")

    entry = (baseline.get("runners") or {}).get(tag)
    result: dict[str, Any] = {
        "schema": 2,
        "kind": "thyme-bench-gate",
        "generated_at": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "runner_tag": tag,
        "host": host,
        "kinds": [doc["kind"] for doc in docs],
        "tolerance_pct": tolerances["ns_per_op"],
        "tolerance_p99_pct": tolerances["p99"],
        "tolerance_p999_pct": tolerances["p999"],
        "baseline_recorded_at": entry.get("recorded_at") if entry else None,
        "baseline_git_sha": entry.get("git_sha") if entry else None,
        "rows": [],
        "status": "no-baseline",
    }

    bad_runs = [doc for doc in docs if not doc["ok"]]
    if bad_runs:
        result["status"] = "fail"
        result["note"] = (
            "bench document reported a failed correctness check: "
            + ", ".join(str(doc["path"]) for doc in bad_runs))

    if entry is None:
        rows: list[dict] = []
        for doc in docs:
            for name, metric in doc["metrics"].items():
                rows.append({
                    "metric": name, "baseline_ns_per_op": None,
                    "run_ns_per_op": metric["ns_per_op"], "delta_pct": None,
                    "gbps": derived_gbps(metric),
                    "baseline_p99_ns_per_op": None,
                    "run_p99_ns_per_op": metric.get("p99_ns_per_op"),
                    "delta_p99_pct": None, "p99_status": "new",
                    "baseline_p999_ns_per_op": None,
                    "run_p999_ns_per_op": metric.get("p999_ns_per_op"),
                    "delta_p999_pct": None, "p999_status": "new",
                    "status": "new",
                })
        result["rows"] = rows
        result.setdefault("note", (
            f"no baseline for runner {tag!r}; run with --write-baseline on "
            "this runner to record one"))
        return result

    mismatch = host_mismatch(docs[0], entry)
    if mismatch is not None:
        # A failed correctness check is a real failure even on a host whose
        # numbers are not comparable; only the timing comparison is skipped.
        result["status"] = (
            "fail" if (require_host_match or bad_runs) else "host-mismatch")
        result["note"] = f"host mismatch: {mismatch}"
        return result

    rows, failed = _compare_metrics(docs, entry, tolerances)
    result["rows"] = rows
    if result["status"] != "fail":
        result["status"] = "fail" if failed else "pass"
    return result


def compare(run: dict, baseline: dict, tolerance: float,
            require_host_match: bool = False,
            tolerance_p99: float | None = None,
            tolerance_p999: float | None = None,
            path: pathlib.Path | None = None) -> dict:
    """Single-document convenience wrapper (used by gen_acceptance.py)."""
    doc = normalize_run(run, path or pathlib.Path("<run>"))
    tolerances = tolerance_set(baseline, tolerance, tolerance_p99,
                               tolerance_p999)
    return compare_documents([doc], baseline, tolerances, require_host_match)


def _fmt(value: float | None, digits: int = 3) -> str:
    return "-" if value is None else f"{value:.{digits}f}"


def _fmt_delta(value: float | None) -> str:
    return "-" if value is None else f"{value:+.1f}%"


def print_table(result: dict) -> None:
    print(f"bench gate: runner={result['runner_tag']} "
          f"tolerance ns/op=+{result['tolerance_pct']:.1f}% "
          f"p99=+{result['tolerance_p99_pct']:.1f}% "
          f"p999=+{result['tolerance_p999_pct']:.1f}%")
    if result.get("baseline_recorded_at"):
        print(f"baseline: recorded {result['baseline_recorded_at']} "
              f"at {result['baseline_git_sha']}")
    header = (f"{'Metric':<38} {'Base ns/op':>12} {'Run ns/op':>12} "
              f"{'d ns':>9} {'d p99':>9} {'d p999':>9} {'GB/s':>7}  Status")
    print(header)
    print("-" * len(header))
    for row in result["rows"]:
        print(f"{row['metric']:<38} "
              f"{_fmt(row['baseline_ns_per_op']):>12} "
              f"{_fmt(row['run_ns_per_op']):>12} "
              f"{_fmt_delta(row['delta_pct']):>9} "
              f"{_fmt_delta(row['delta_p99_pct']):>9} "
              f"{_fmt_delta(row['delta_p999_pct']):>9} "
              f"{_fmt(row['gbps'], 2):>7}  {row['status'].upper()}")
    if result.get("note"):
        print(f"\nnote: {result['note']}")


def write_baseline(path: pathlib.Path, runs: list[dict],
                   baseline: dict | None) -> None:
    """Record one or more runs (same runner) into the baseline, merging keys."""
    docs = [normalize_run(run, pathlib.Path("<run>")) for run in runs]
    tag = docs[0]["runner_tag"]
    host = docs[0]["host"]
    for doc in docs[1:]:
        if doc["runner_tag"] != tag:
            raise SystemExit(
                f"refusing to mix runner tags in one baseline write: "
                f"{tag!r} and {doc['runner_tag']!r}")
        if doc["host"] != host:
            raise SystemExit(
                f"refusing to mix hosts in one baseline write: {host!r} and "
                f"{doc['host']!r}")
    for doc in docs:
        if not doc["ok"]:
            raise SystemExit(
                f"refusing to record a baseline from a failed bench run: "
                f"{doc['path']}")

    previous = baseline or {}
    doc_out = {
        "schema": 2,
        "comment": (
            "PERF-06 per-runner bench baselines. Keyed by runner tag and "
            "recording host: x86_64-local is the development machine and is "
            "reference-only evidence; x86_64-ci is the GitLab runner and "
            "records its own with --write-baseline (until then the gate "
            "reports no-baseline and skips). Never copy numbers between "
            "runners. tolerance_ns_per_op_pct / tolerance_p99_pct / "
            "tolerance_p999_pct are the allowed regressions for the "
            "steady-state, p99 and p999 statistics (see "
            "docs/guides/bench-baselines.md)."),
        "tolerance_ns_per_op_pct": previous.get(
            "tolerance_ns_per_op_pct", DEFAULT_TOLERANCE_PCT),
        "tolerance_p99_pct": previous.get(
            "tolerance_p99_pct", DEFAULT_P99_TOLERANCE_PCT),
        "tolerance_p999_pct": previous.get(
            "tolerance_p999_pct", DEFAULT_P999_TOLERANCE_PCT),
        "unset_runners": previous.get("unset_runners", {
            "x86_64-ci": (
                "no baseline recorded yet: nightly-bench reports "
                "no-baseline and skips until the first CI run is recorded "
                "with --write-baseline on the x86_64-ci runner"),
        }),
        "runners": dict(previous.get("runners") or {}),
    }
    runners = doc_out["runners"]
    entry = runners.setdefault(tag, {})
    merged = entry.setdefault("benchmarks", {})
    for run in docs:
        for name, metric in run["metrics"].items():
            merged[name] = {
                "bench": run["kind"],
                "ns_per_op": round(float(metric["ns_per_op"]), 4),
                "p99_ns_per_op": (
                    round(float(metric["p99_ns_per_op"]), 4)
                    if metric["p99_ns_per_op"] is not None else None),
                "p999_ns_per_op": (
                    round(float(metric["p999_ns_per_op"]), 4)
                    if metric["p999_ns_per_op"] is not None else None),
                "gbps": (round(float(metric["gbps"]), 3)
                         if metric.get("gbps") is not None else None),
                "bytes_per_op": metric.get("bytes_per_op", 0),
                "iterations": metric.get("iterations", 0),
            }
    def first(key: str) -> Any:
        return next((run.get(key) for run in runs if run.get(key)), None)

    entry.update({
        "recorded_at": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        # A developer-box baseline is evidence only; a runner baseline is the
        # gate. The gate behaviour is identical, the label documents intent.
        "role": "reference-only" if tag.endswith("-local") else "gate",
        "git_sha": first("git_sha"),
        "toolchain": first("toolchain"),
        "build_type": first("build_type"),
        "hostname": host.get("hostname"),
        "cpu": first("cpu"),
        "host": host,
    })
    path.write_text(json.dumps(doc_out, indent=2) + "\n", encoding="utf-8")
    print(f"baseline written to {path} for runner {tag!r} "
          f"({len(merged)} metrics)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run", action="append", required=True,
                        help="bench JSON output of the fresh run "
                             "(repeat for core/object-pool/trigger-seal)")
    parser.add_argument("--baseline", default=str(DEFAULT_BASELINE),
                        help="baseline file (default: tools/ci/bench_baseline.json)")
    parser.add_argument("--tolerance", type=float,
                        help="allowed ns/op regression in percent "
                             f"(default: baseline value, else "
                             f"{DEFAULT_TOLERANCE_PCT:g})")
    parser.add_argument("--tolerance-p99", type=float,
                        help="allowed p99 regression in percent "
                             f"(default: baseline value, else "
                             f"{DEFAULT_P99_TOLERANCE_PCT:g})")
    parser.add_argument("--tolerance-p999", type=float,
                        help="allowed p999 regression in percent "
                             f"(default: baseline value, else "
                             f"{DEFAULT_P999_TOLERANCE_PCT:g})")
    parser.add_argument("--write-baseline", action="store_true",
                        help="record these runs as the baseline for their "
                             "runner tag")
    parser.add_argument("--require-baseline", action="store_true",
                        help="fail instead of skipping when the runner tag has "
                             "no recorded baseline")
    parser.add_argument("--require-host-match", action="store_true",
                        help="fail on hostname/arch mismatch instead of skipping")
    parser.add_argument("--json-out", help="write the structured gate result")
    args = parser.parse_args()

    runs = []
    run_paths = []
    for raw in args.run:
        path = pathlib.Path(raw).resolve()
        runs.append(load_json(path))
        run_paths.append(path)

    baseline_path = pathlib.Path(args.baseline).resolve()
    baseline = None
    if baseline_path.is_file():
        baseline = load_json(baseline_path)

    if args.write_baseline:
        write_baseline(baseline_path, runs, baseline)
        return 0

    docs = [normalize_run(run, path) for run, path in zip(runs, run_paths)]
    baseline = baseline or {}
    tolerances = tolerance_set(baseline, args.tolerance, args.tolerance_p99,
                               args.tolerance_p999)
    result = compare_documents(docs, baseline, tolerances,
                               args.require_host_match)
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
        print(f"\nwarning: {result['note']}; gate skipped. Record a baseline "
              "on this host or pass --require-host-match.", file=sys.stderr)
        return 0
    if result["status"] == "fail":
        failed = [f"{row['metric']}.{stat}" for row in result["rows"]
                  for stat, status in (
                      ("ns_per_op", row["status"]),
                      ("p99", row.get("p99_status")),
                      ("p999", row.get("p999_status")))
                  if status in FAILING_STATUSES]
        print(f"\nbench gate FAILED: {', '.join(failed) or 'failed run'}",
              file=sys.stderr)
        return 1
    print("\nbench gate PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
