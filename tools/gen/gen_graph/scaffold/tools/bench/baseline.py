#!/usr/bin/env python3
"""Collect nightly bench + ctest metrics into baseline/<runner_fingerprint>.json (M7.3).

Usage:
  python3 tools/bench/baseline.py --build-dir build --seconds 60

CI (GitLab schedule):
  nightly:baseline job in .github/workflows/ci.yml uploads baseline/ artifacts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
MAX_HISTORY = 7


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


def run_cmd(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    timeout: int | None = None,
    capture_stdout: bool = False,
) -> dict[str, Any]:
    started = time.time()
    try:
        proc = subprocess.run(
            cmd,
            cwd=cwd,
            timeout=timeout,
            text=True,
            capture_output=capture_stdout,
            check=False,
        )
        return {
            "cmd": cmd,
            "exit_code": proc.returncode,
            "duration_s": round(time.time() - started, 3),
            "stdout": proc.stdout if capture_stdout else None,
            "stderr_tail": (proc.stderr or "")[-4000:] if capture_stdout else None,
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "cmd": cmd,
            "exit_code": -1,
            "duration_s": round(time.time() - started, 3),
            "timeout": True,
            "stderr_tail": (exc.stderr or "")[-4000:] if exc.stderr else None,
        }


def load_bench_json(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        with path.open(encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        return {"parse_error": str(exc)}


def run_bench(build_dir: Path, name: str, exe: Path, seconds: int) -> dict[str, Any]:
    json_path = build_dir / f"baseline_{name}.json"
    csv_path = build_dir / f"baseline_{name}.csv"
    result = run_cmd(
        [str(exe), "--seconds", str(seconds), "--out", str(csv_path), "--json", str(json_path)],
        cwd=build_dir,
        timeout=seconds + 120,
    )
    result["name"] = name
    result["json_path"] = str(json_path)
    result["summary"] = load_bench_json(json_path)
    return result


def run_ctest(build_dir: Path) -> dict[str, Any]:
    log_path = build_dir / "baseline_ctest.log"
    proc = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--output-on-failure", "-j", str(os.cpu_count() or 2)],
        text=True,
        capture_output=True,
        check=False,
    )
    log_path.write_text(proc.stdout + proc.stderr, encoding="utf-8")
    passed = failed = 0
    for line in (proc.stdout + proc.stderr).splitlines():
        line = line.strip()
        if line.endswith(" tests passed") and "tests passed" in line:
            # e.g. "98% tests passed, 2 tests failed out of 100"
            parts = line.replace(",", "").split()
            for i, tok in enumerate(parts):
                if tok == "passed," and i > 0:
                    try:
                        passed = int(parts[i - 1].replace("%", ""))
                    except ValueError:
                        pass
                if tok == "failed" and i > 0:
                    try:
                        failed = int(parts[i - 1])
                    except ValueError:
                        pass
    return {
        "exit_code": proc.returncode,
        "passed_hint": passed,
        "failed_hint": failed,
        "log_path": str(log_path),
    }


def run_eval_chains(build_dir: Path, seconds_per_scenario: int) -> dict[str, Any]:
    ad_stack = build_dir / "ad_stack_twin"
    if not ad_stack.is_file():
        ad_stack = build_dir / "ad_stack"
    recorder = build_dir / "recorder_twin"
    if not recorder.is_file():
        recorder = build_dir / "recorder"
    eval_chains = build_dir / "eval_chains"
    if not eval_chains.is_file():
        return {"status": "skipped", "reason": "eval_chains binary missing"}
    result = run_cmd(
        [str(eval_chains), str(ad_stack), str(recorder), str(seconds_per_scenario)],
        cwd=build_dir,
        timeout=seconds_per_scenario * 6 + 300,
        capture_stdout=True,
    )
    payload: dict[str, Any] = {
        "status": "ok" if result["exit_code"] == 0 else "failed",
        "exit_code": result["exit_code"],
        "duration_s": result["duration_s"],
    }
    if result.get("stdout"):
        try:
            payload["report"] = json.loads(result["stdout"])
        except json.JSONDecodeError:
            payload["stdout_tail"] = result["stdout"][-2000:]
    return payload


def run_tsan_smoke(build_dir: Path) -> dict[str, Any]:
    proc = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "-R", "tsan", "--output-on-failure"],
        text=True,
        capture_output=True,
        check=False,
    )
    if "No tests were found" in proc.stdout + proc.stderr:
        return {"status": "skipped", "reason": "no tsan tests in build"}
    return {
        "status": "ok" if proc.returncode == 0 else "failed",
        "exit_code": proc.returncode,
    }


def collect_run(args: argparse.Namespace) -> dict[str, Any]:
    build_dir = Path(args.build_dir).resolve()
    fp = runner_fingerprint()
    run: dict[str, Any] = {
        "collected_at": datetime.now(timezone.utc).isoformat(),
        "schema_version": SCHEMA_VERSION,
        "runner_fingerprint": fp,
        "runner": {
            "hostname": platform.node(),
            "platform": platform.platform(),
            "cpu_count": os.cpu_count(),
            "ci_pipeline_id": os.environ.get("CI_PIPELINE_ID"),
            "ci_job_id": os.environ.get("CI_JOB_ID"),
        },
        "bench_seconds": args.seconds,
        "bench": {},
        "ctest": {},
        "eval_chains": {},
        "tsan": {},
    }

    cyclic = build_dir / "bench_ad_stack"
    twin = build_dir / "bench_ad_stack_twin"
    if cyclic.is_file():
        run["bench"]["cyclic"] = run_bench(build_dir, "cyclic", cyclic, args.seconds)
    else:
        run["bench"]["cyclic"] = {"status": "skipped", "reason": "bench_ad_stack missing"}

    if twin.is_file():
        run["bench"]["twin"] = run_bench(build_dir, "twin", twin, args.seconds)
    else:
        run["bench"]["twin"] = {"status": "skipped", "reason": "bench_ad_stack_twin missing"}

    if not args.skip_ctest:
        run["ctest"] = run_ctest(build_dir)

    if not args.skip_eval:
        run["eval_chains"] = run_eval_chains(build_dir, args.eval_seconds)

    if not args.skip_tsan:
        run["tsan"] = run_tsan_smoke(build_dir)

    return run


def merge_history(existing: dict[str, Any] | None, latest: dict[str, Any]) -> dict[str, Any]:
    if existing is None:
        return {
            "schema_version": SCHEMA_VERSION,
            "runner_fingerprint": latest["runner_fingerprint"],
            "latest": latest,
            "history": [],
        }
    history = existing.get("history", [])
    if "latest" in existing:
        history = [existing["latest"]] + history
    history = history[: MAX_HISTORY - 1]
    return {
        "schema_version": SCHEMA_VERSION,
        "runner_fingerprint": latest["runner_fingerprint"],
        "latest": latest,
        "history": history,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="M7.3 nightly baseline collector")
    parser.add_argument("--build-dir", default="build", help="CMake build directory")
    parser.add_argument("--seconds", type=int, default=60, help="bench duration per pipeline")
    parser.add_argument("--eval-seconds", type=int, default=15, help="eval_chains seconds per scenario")
    parser.add_argument("--out-dir", default="baseline", help="output directory for JSON")
    parser.add_argument("--skip-ctest", action="store_true")
    parser.add_argument("--skip-eval", action="store_true")
    parser.add_argument("--skip-tsan", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="print path only, skip heavy runs")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    fp = runner_fingerprint()
    out_path = out_dir / f"{fp}.json"

    if args.dry_run:
        print(out_path)
        return 0

    latest = collect_run(args)
    existing = None
    if out_path.is_file():
        try:
            with out_path.open(encoding="utf-8") as f:
                existing = json.load(f)
        except (OSError, json.JSONDecodeError):
            existing = None

    doc = merge_history(existing, latest)
    with out_path.open("w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")

    print(f"baseline written: {out_path}")
    return exit_code_for_run(latest)


def exit_code_for_run(
    run: dict[str, Any],
    *,
    baseline_strict: bool | None = None,
    eval_strict: bool | None = None,
) -> int:
    """Return process exit code for a collected nightly run (testable)."""
    if baseline_strict is None:
        baseline_strict = os.environ.get("BASELINE_STRICT") == "1"
    if eval_strict is None:
        eval_strict = os.environ.get("EVAL_CHAINS_STRICT", "0") == "1"

    if baseline_strict:
        cyclic = run.get("bench", {}).get("cyclic", {})
        if cyclic.get("exit_code", 0) != 0:
            return 1

    if eval_strict:
        ec = run.get("eval_chains", {})
        if ec.get("status") == "failed":
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
