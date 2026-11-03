#!/usr/bin/env python3
"""Run eval_chains under the OBS-10 cpu/io/irq loads and write a compact JSON.

`eval_chains` (generated app fixture, tools/gen/gen_graph/testdata/eval_chains)
already emits the full report on stdout: per-node work/latency, per-edge
latency, per-frame E2E series and /proc resources. That document is large and
contains per-sample arrays that are useless for a nightly baseline, so this
driver keeps the raw reports as artifacts and normalizes the gated metrics
into ``thyme-eval-chains-run``:

  scenarios.<load>.metrics:
    frames_per_sec          completed E2E frame joins per second (higher is better)
    e2e_p95_ns              frame-joined latency p95 (lower is better)

``frames_per_sec`` is used instead of the raw join count so the baseline stays
valid when ``--seconds`` changes; the raw count and broken-join count are kept
alongside the metrics for context.

The raw report keeps p50/p99, per-node work, per-edge latency and the full
series for investigation; only throughput + E2E p95 are baseline-gated.

Loads are implemented inside eval_chains itself (pinned spinners, /tmp
write+fsync, timerfd + sched_yield); no external stress tool is required. A
load process that dies early is reported as DEGRADED and carried in the JSON
(``load_alive``/``load_requested``) instead of failing the job.

Usage:
  python3 tools/ci/run_eval_chains.py \
    --eval-chains build-eval-chains/build/eval_chains \
    --seconds 10 --out eval-chains-report/eval-chains.json \
    --raw-dir eval-chains-report/raw
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any

SCENARIOS = ("cpu", "io", "irq")
SCHEMA = 1
KIND = "thyme-eval-chains-run"


def _run(cmd: list[str], timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd, text=True, capture_output=True, timeout=timeout, check=False)


def _git_sha(repo: Path) -> str:
    try:
        proc = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
            text=True, capture_output=True, check=False)
    except OSError:
        return ""
    return proc.stdout.strip() if proc.returncode == 0 else ""


def _cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.lower().startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def host_info() -> dict[str, Any]:
    return {
        "hostname": platform.node(),
        "machine": platform.machine(),
        "cpu": _cpu_model(),
        "cpu_count": os.cpu_count() or 0,
    }


def default_runner_tag() -> str:
    return os.environ.get(
        "THYME_EVAL_CHAINS_RUNNER_TAG",
        "x86_64-ci" if os.environ.get("CI") else "x86_64-local",
    )


def _int(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def summarize(report: dict[str, Any], load: str) -> dict[str, Any]:
    """Reduce one eval_chains report to the gated metric set."""
    if report.get("load") != load:
        raise ValueError(
            f"report load {report.get('load')!r} != requested {load!r}")

    e2e = report.get("e2e") or {}
    seconds = _int(report.get("seconds"))
    frames = _int(e2e.get("n"))
    broken = _int(e2e.get("broken"))
    # Gate throughput + E2E p95 only. The raw report keeps p50/p99, per-node
    # work and per-edge series for investigation, but on a shared host those
    # are bimodal (a hop either catches the next period or the one after), so a
    # tight gate on them would flake. The tolerance is correspondingly loose;
    # see docs/guides/eval-chains.md.
    # Throughput is per second so the baseline survives a --seconds change.
    metrics: dict[str, float] = {
        "frames_per_sec": round(frames / seconds, 3) if seconds > 0 else 0.0,
        "e2e_p95_ns": _int(e2e.get("p95")),
    }

    requested = report.get("load_requested")
    alive = report.get("load_alive")
    load_reported = requested is not None and alive is not None
    if not load_reported:
        requested = 1 if load != "baseline" else 0
        alive = requested
    requested_i = _int(requested)
    alive_i = _int(alive)
    notes: list[str] = []
    degraded = requested_i > 0 and alive_i < requested_i
    if degraded:
        notes.append(
            f"load degraded: {alive_i}/{requested_i} load processes alive")
    if not load_reported:
        notes.append("eval_chains did not report load status (older build)")

    broken_pct = (100.0 * broken / frames) if frames > 0 else 0.0
    status = "ok"
    if frames == 0:
        status = "failed"
        notes.append("no completed E2E frames")
    return {
        "status": status,
        "degraded": degraded,
        "notes": notes,
        "seconds": seconds,
        "frames": frames,
        "broken": broken,
        "broken_pct": round(broken_pct, 3),
        "metrics": metrics,
    }


def run_scenario(eval_chains: Path, load: str, seconds: int,
                 chain: str, timeout: int) -> tuple[dict[str, Any], dict[str, Any]]:
    # inproc keeps the measurement single-process: the loads and the chain
    # share one scheduler, which is what the cpu/io/irq scenarios model.
    cmd = [str(eval_chains), "--mode", "inproc", "--seconds", str(seconds),
           "--chain", chain, "--load", load]
    print(f"[run_eval_chains] load={load} cmd={' '.join(cmd)}", file=sys.stderr)
    try:
        proc = _run(cmd, timeout)
    except subprocess.TimeoutExpired:
        return ({"status": "failed", "degraded": False,
                 "notes": [f"eval_chains timed out after {timeout}s"],
                 "seconds": seconds, "frames": 0, "broken": 0,
                 "broken_pct": 0.0, "metrics": {}}, {})

    for line in (proc.stderr or "").splitlines():
        if line.strip():
            print(f"[eval_chains:{load}] {line}", file=sys.stderr)

    if proc.returncode != 0:
        return ({"status": "failed", "degraded": False,
                 "notes": [f"eval_chains exited {proc.returncode}"],
                 "seconds": seconds, "frames": 0, "broken": 0,
                 "broken_pct": 0.0, "metrics": {}}, {})
    try:
        raw = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        return ({"status": "failed", "degraded": False,
                 "notes": [f"invalid JSON on stdout: {exc}"],
                 "seconds": seconds, "frames": 0, "broken": 0,
                 "broken_pct": 0.0, "metrics": {}}, {})
    if not isinstance(raw, dict):
        return ({"status": "failed", "degraded": False,
                 "notes": ["eval_chains stdout is not a JSON object"],
                 "seconds": seconds, "frames": 0, "broken": 0,
                 "broken_pct": 0.0, "metrics": {}}, {})
    try:
        scenario = summarize(raw, load)
    except ValueError as exc:
        return ({"status": "failed", "degraded": False,
                 "notes": [str(exc)],
                 "seconds": seconds, "frames": 0, "broken": 0,
                 "broken_pct": 0.0, "metrics": {}}, raw)
    if scenario["degraded"]:
        for note in scenario["notes"]:
            print(f"[run_eval_chains] WARNING load={load}: {note}",
                  file=sys.stderr)
    print(f"[run_eval_chains] load={load} status={scenario['status']} "
          f"frames={scenario['frames']} broken={scenario['broken']} "
          f"e2e_p95={scenario['metrics'].get('e2e_p95_ns', 0)}ns",
          file=sys.stderr)
    return scenario, raw


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--eval-chains", default="build-eval-chains/build/eval_chains",
                        help="eval_chains binary")
    parser.add_argument("--out", default="eval-chains-report/eval-chains.json",
                        help="normalized run JSON")
    parser.add_argument("--raw-dir", help="directory for the raw per-scenario JSON")
    parser.add_argument("--seconds", type=int, default=10,
                        help="seconds per scenario (default 10)")
    parser.add_argument("--scenarios", default=",".join(SCENARIOS),
                        help="comma-separated loads (default cpu,io,irq)")
    parser.add_argument("--chain", default="head,mid,tail")
    parser.add_argument("--timeout", type=int,
                        help="per-scenario timeout (default seconds + 180)")
    parser.add_argument("--max-broken-pct", type=float, default=5.0,
                        help="fail when more E2E joins are broken (default 5%%)")
    parser.add_argument("--runner-tag", default=default_runner_tag())
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--git-sha", default=_git_sha(repo))
    args = parser.parse_args()

    eval_chains = Path(args.eval_chains).resolve()
    if not eval_chains.is_file() or not os.access(eval_chains, os.X_OK):
        print(f"run_eval_chains: eval_chains not executable: {eval_chains}",
              file=sys.stderr)
        return 2
    if args.seconds <= 0:
        print("run_eval_chains: --seconds must be positive", file=sys.stderr)
        return 2

    scenarios = [s.strip() for s in args.scenarios.split(",") if s.strip()]
    unknown = [s for s in scenarios if s not in SCENARIOS]
    if unknown:
        print(f"run_eval_chains: unknown scenario(s): {','.join(unknown)}",
              file=sys.stderr)
        return 2

    raw_dir = Path(args.raw_dir) if args.raw_dir else None
    if raw_dir is not None:
        raw_dir.mkdir(parents=True, exist_ok=True)

    timeout = args.timeout or (args.seconds + 180)
    results: dict[str, dict[str, Any]] = {}
    failed = False
    for load in scenarios:
        scenario, raw = run_scenario(
            eval_chains, load, args.seconds, args.chain, timeout)
        if raw and raw_dir is not None:
            (raw_dir / f"{load}.json").write_text(
                json.dumps(raw, indent=2) + "\n", encoding="utf-8")
        if scenario["status"] != "ok":
            failed = True
        elif scenario["broken_pct"] > args.max_broken_pct:
            scenario["status"] = "failed"
            scenario["notes"].append(
                f"broken E2E joins {scenario['broken_pct']:.2f}% "
                f"> {args.max_broken_pct:.2f}%")
            failed = True
        results[load] = scenario

    # eval_chains kills its io writers with SIGKILL, so the last file may not
    # have been unlinked. Remove this run's leftovers; the prefix is unique to
    # the io load.
    for leftover in Path("/tmp").glob("thyme_eval_io_*"):
        try:
            leftover.unlink()
        except OSError:
            pass

    doc = {
        "kind": KIND,
        "schema": SCHEMA,
        "runner_tag": args.runner_tag,
        "recorded_at": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_sha": args.git_sha,
        "build_type": args.build_type,
        "host": host_info(),
        "seconds_per_scenario": args.seconds,
        "chain": [c for c in args.chain.split(",") if c],
        "scenarios": results,
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    tmp.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    tmp.replace(out)
    print(f"[run_eval_chains] wrote {out}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
