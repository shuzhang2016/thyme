#!/usr/bin/env python3
"""Unit tests for baseline_gate.py (M7.4)."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# Allow import from tools/bench/
sys.path.insert(0, str(Path(__file__).resolve().parent))

import baseline_gate as bg  # noqa: E402

# baseline collector exit logic (M3.3+ eval strict)
sys.path.insert(0, str(Path(__file__).resolve().parent))
import baseline as bl  # noqa: E402


def _summary(
    *,
    drops: int = 0,
    wake_p99: int = 1000,
    work_p99: int = 2000,
    lat_p99: int = 50_000_000,
    name: str = "perception",
) -> dict:
    return {
        "seconds": 60,
        "total_drops": drops,
        "threads": [
            {
                "name": name,
                "period_hz": 50,
                "loops": 3000,
                "p50_wake_err_ns": 500,
                "p99_wake_err_ns": wake_p99,
                "max_wake_err_ns": wake_p99 + 100,
                "p50_work_ns": 1000,
                "p99_work_ns": work_p99,
                "max_work_ns": work_p99 + 100,
                "drops": drops,
                "deadline_miss": 0,
            }
        ],
        "pipeline": {"samples": 100, "p50_lat_ns": 40_000_000, "p99_lat_ns": lat_p99, "max_lat_ns": lat_p99 + 1},
    }


def _run(*, drops: int = 0, wake_p99: int = 1000, work_p99: int = 2000, lat_p99: int = 50_000_000) -> dict:
    return {
        "bench": {
            "cyclic": {
                "exit_code": 0 if drops == 0 else 1,
                "summary": _summary(drops=drops, wake_p99=wake_p99, work_p99=work_p99, lat_p99=lat_p99),
            }
        }
    }


class BaselineGateTests(unittest.TestCase):
    def test_not_ready_skips(self) -> None:
        doc = {"latest": _run(), "history": [_run() for _ in range(3)]}
        rc = bg.run_check(doc, _summary(), min_runs=7, slack=1.25, bench_soft=False)
        self.assertEqual(rc, 0)

    def test_ready_passes_within_threshold(self) -> None:
        history = [_run(wake_p99=800, work_p99=1600, lat_p99=40_000_000) for _ in range(6)]
        doc = {"latest": _run(wake_p99=900, work_p99=1700, lat_p99=45_000_000), "history": history}
        rc = bg.run_check(doc, _summary(wake_p99=900, work_p99=1700, lat_p99=45_000_000), min_runs=7, slack=1.25, bench_soft=False)
        self.assertEqual(rc, 0)

    def test_perf_fail_hard(self) -> None:
        history = [_run(wake_p99=800) for _ in range(6)]
        doc = {"latest": _run(wake_p99=800), "history": history}
        # max wake 800 * 1.25 = 1000; 1500 exceeds
        rc = bg.run_check(doc, _summary(wake_p99=1500), min_runs=7, slack=1.25, bench_soft=False)
        self.assertEqual(rc, 1)

    def test_perf_soft_downgrade(self) -> None:
        history = [_run(wake_p99=800) for _ in range(6)]
        doc = {"latest": _run(wake_p99=800), "history": history}
        rc = bg.run_check(doc, _summary(wake_p99=1500), min_runs=7, slack=1.25, bench_soft=True)
        self.assertEqual(rc, 0)

    def test_drops_always_fail(self) -> None:
        history = [_run() for _ in range(6)]
        doc = {"latest": _run(), "history": history}
        rc = bg.run_check(doc, _summary(drops=3), min_runs=7, slack=1.25, bench_soft=True)
        self.assertEqual(rc, 1)

    def test_eval_strict_fail(self) -> None:
        run = {"eval_chains": {"status": "failed", "exit_code": 1}}
        self.assertEqual(bl.exit_code_for_run(run, eval_strict=True), 1)

    def test_eval_strict_skip_ok(self) -> None:
        run = {"eval_chains": {"status": "skipped"}}
        self.assertEqual(bl.exit_code_for_run(run, eval_strict=True), 0)

    def test_cli_missing_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(__file__).resolve().parent / "baseline_gate.py"
            proc = subprocess.run(
                [sys.executable, str(script), "--baseline-dir", tmp, "--check-latest"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0)
            self.assertIn("GATE_SKIP", proc.stderr)


if __name__ == "__main__":
    unittest.main()
