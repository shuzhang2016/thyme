#!/usr/bin/env python3
"""Unit tests for tools/ci/check_eval_chains.py (OBS-10).

Plain ``unittest`` with in-memory fixture JSON; registered as
``test_check_eval_chains`` (labels ``unit;mr_gate``) in the root CMakeLists.
"""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_eval_chains as cec  # noqa: E402
import run_eval_chains as rec  # noqa: E402

TAG = "x86_64-local"


def _metrics(**overrides: int) -> dict[str, int]:
    base = {
        "frames_per_sec": 100.0,
        "e2e_p95_ns": 1_500,
        "edge.hop1.lat_p95_ns": 700,
        "edge.hop2.lat_p95_ns": 800,
    }
    base.update(overrides)
    return base


def _scenario(metrics: dict[str, int] | None = None,
              status: str = "ok") -> dict:
    return {
        "status": status,
        "degraded": False,
        "notes": [],
        "seconds": 5,
        "frames": 500,
        "broken": 0,
        "broken_pct": 0.0,
        "metrics": metrics if metrics is not None else _metrics(),
    }


def _run(hostname: str = "host-a", machine: str = "x86_64",
         scenarios: dict[str, dict] | None = None,
         tag: str = TAG) -> dict:
    return {
        "kind": cec.KIND,
        "schema": 1,
        "runner_tag": tag,
        "recorded_at": "2026-10-24T00:00:00Z",
        "git_sha": "abc1234",
        "build_type": "Release",
        "host": {"hostname": hostname, "machine": machine},
        "seconds_per_scenario": 5,
        "chain": ["head", "mid", "tail"],
        "scenarios": scenarios or {"cpu": _scenario()},
    }


def _baseline(run: dict, hostname: str = "host-a",
              machine: str = "x86_64") -> dict:
    return {
        "comment": "fixture",
        "tolerance_pct": 25.0,
        "runners": {
            run["runner_tag"]: {
                "recorded_at": "2026-10-24T00:00:00Z",
                "git_sha": "abc1234",
                "build_type": "Release",
                "host": {"hostname": hostname, "machine": machine},
                "scenarios": copy.deepcopy(run["scenarios"]),
            }
        },
    }


class CompareTests(unittest.TestCase):
    def test_pass_within_tolerance(self) -> None:
        base_run = _run()
        current = _run(scenarios={
            "cpu": _scenario(_metrics(e2e_p95_ns=1_800))})  # +20%
        result = cec.compare(current, _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "pass")
        self.assertTrue(all(r["status"] == "pass" for r in result["rows"]))

    def test_fail_latency_regression(self) -> None:
        base_run = _run()
        current = _run(scenarios={
            "cpu": _scenario(_metrics(e2e_p95_ns=1_950))})  # +30%
        result = cec.compare(current, _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "fail")
        failing = [r for r in result["rows"] if r["status"] == "fail"]
        self.assertEqual([r["metric"] for r in failing], ["e2e_p95_ns"])

    def test_throughput_higher_is_better(self) -> None:
        base_run = _run()
        ok = _run(scenarios={"cpu": _scenario(_metrics(frames_per_sec=80.0))})
        bad = _run(scenarios={"cpu": _scenario(_metrics(frames_per_sec=70.0))})
        self.assertEqual(
            cec.compare(ok, _baseline(base_run), 25.0, False)["status"], "pass")
        result = cec.compare(bad, _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "fail")
        self.assertEqual(
            [r["metric"] for r in result["rows"] if r["status"] == "fail"],
            ["frames_per_sec"])

    def test_missing_metric_fails(self) -> None:
        base_run = _run()
        metrics = _metrics()
        del metrics["e2e_p95_ns"]
        result = cec.compare(
            _run(scenarios={"cpu": _scenario(metrics)}),
            _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "fail")
        missing = [r for r in result["rows"] if r["status"] == "missing"]
        self.assertEqual([r["metric"] for r in missing], ["e2e_p95_ns"])

    def test_new_metric_is_not_gated(self) -> None:
        base_run = _run()
        metrics = _metrics()
        metrics["edge.hop1.lat_p50_ns"] = 900
        result = cec.compare(
            _run(scenarios={"cpu": _scenario(metrics)}),
            _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "pass")
        self.assertIn("new", [r["status"] for r in result["rows"]])

    def test_missing_scenario_fails(self) -> None:
        base_run = _run(scenarios={"cpu": _scenario(), "io": _scenario()})
        result = cec.compare(
            _run(scenarios={"cpu": _scenario()}),
            _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "fail")
        self.assertIn("missing-scenario",
                      [r["status"] for r in result["rows"]])

    def test_failed_run_scenario_fails(self) -> None:
        base_run = _run()
        result = cec.compare(
            _run(scenarios={"cpu": _scenario(status="failed")}),
            _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "fail")
        self.assertIn("failed-run", [r["status"] for r in result["rows"]])

    def test_hostname_mismatch_skips_by_default(self) -> None:
        base_run = _run()
        result = cec.compare(
            _run(hostname="host-b"), _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "host-mismatch")
        self.assertIn("hostname", result["note"])

    def test_host_mismatch_can_be_required(self) -> None:
        base_run = _run()
        result = cec.compare(
            _run(hostname="host-b"), _baseline(base_run), 25.0, True)
        self.assertEqual(result["status"], "fail")

    def test_arch_mismatch(self) -> None:
        base_run = _run()
        result = cec.compare(
            _run(machine="aarch64"), _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "host-mismatch")
        self.assertIn("arch", result["note"])

    def test_no_baseline_for_runner(self) -> None:
        result = cec.compare(
            _run(tag="x86_64-ci"), _baseline(_run()), 25.0, False)
        self.assertEqual(result["status"], "no-baseline")
        self.assertTrue(all(r["status"] == "new" for r in result["rows"]))

    def test_zero_baseline_latency(self) -> None:
        base_run = _run(scenarios={"cpu": _scenario(_metrics(e2e_p95_ns=0))})
        result = cec.compare(
            _run(scenarios={"cpu": _scenario(_metrics(e2e_p95_ns=10))}),
            _baseline(base_run), 25.0, False)
        self.assertEqual(result["status"], "fail")

    def test_tolerance_override(self) -> None:
        self.assertEqual(cec.tolerance_for({"tolerance_pct": 20.0}, 30.0), 30.0)
        self.assertEqual(cec.tolerance_for({}, None),
                         cec.DEFAULT_TOLERANCE_PCT)


class SummarizeTests(unittest.TestCase):
    """The driver's normalization must not depend on the run duration."""

    def _report(self, **overrides) -> dict:
        report = {
            "load": "cpu",
            "seconds": 10,
            "load_requested": 3,
            "load_alive": 3,
            "e2e": {"n": 2_000, "broken": 0, "p95": 2_000},
        }
        report.update(overrides)
        return report

    def test_throughput_is_normalized_by_seconds(self) -> None:
        ten = rec.summarize(self._report(seconds=10), "cpu")
        five = rec.summarize(self._report(seconds=5, e2e={
            "n": 1_000, "broken": 0, "p95": 2_000}), "cpu")
        self.assertEqual(ten["metrics"]["frames_per_sec"], 200.0)
        self.assertEqual(five["metrics"]["frames_per_sec"], 200.0)

    def test_degraded_load_is_flagged(self) -> None:
        scenario = rec.summarize(
            self._report(load_requested=3, load_alive=1), "cpu")
        self.assertTrue(scenario["degraded"])
        self.assertEqual(scenario["status"], "ok")
        self.assertTrue(any("1/3" in note for note in scenario["notes"]))

    def test_missing_load_status_is_flagged_not_failed(self) -> None:
        report = self._report()
        del report["load_requested"]
        del report["load_alive"]
        scenario = rec.summarize(report, "cpu")
        self.assertFalse(scenario["degraded"])
        self.assertTrue(any("older build" in n for n in scenario["notes"]))

    def test_zero_frames_fails(self) -> None:
        scenario = rec.summarize(
            self._report(e2e={"n": 0, "broken": 0, "p95": 0}), "cpu")
        self.assertEqual(scenario["status"], "failed")

    def test_load_mismatch_raises(self) -> None:
        with self.assertRaises(ValueError):
            rec.summarize(self._report(), "irq")


class ValidationTests(unittest.TestCase):
    def _path(self) -> Path:
        return Path("run.json")

    def test_rejects_wrong_kind(self) -> None:
        run = _run()
        run["kind"] = "something-else"
        with self.assertRaises(SystemExit):
            cec.validate_run(run, self._path())

    def test_rejects_missing_tag(self) -> None:
        run = _run()
        del run["runner_tag"]
        with self.assertRaises(SystemExit):
            cec.validate_run(run, self._path())

    def test_rejects_empty_scenarios(self) -> None:
        run = _run()
        run["scenarios"] = {}
        with self.assertRaises(SystemExit):
            cec.validate_run(run, self._path())

    def test_rejects_scenario_without_metrics(self) -> None:
        run = _run(scenarios={"cpu": {"status": "ok"}})
        with self.assertRaises(SystemExit):
            cec.validate_run(run, self._path())


class WriteBaselineTests(unittest.TestCase):
    def test_roundtrip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "baseline.json"
            run = _run()
            cec.write_baseline(path, run, None)
            doc = json.loads(path.read_text(encoding="utf-8"))
            self.assertIn(TAG, doc["runners"])
            result = cec.compare(run, doc, 25.0, False)
            self.assertEqual(result["status"], "pass")

    def test_refuses_failed_scenario(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "baseline.json"
            run = _run(scenarios={"cpu": _scenario(status="failed")})
            with self.assertRaises(SystemExit):
                cec.write_baseline(path, run, None)


class CliTests(unittest.TestCase):
    def _write(self, path: Path, doc: dict) -> None:
        path.write_text(json.dumps(doc), encoding="utf-8")

    def _cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        script = Path(__file__).resolve().parent / "check_eval_chains.py"
        return subprocess.run(
            [sys.executable, str(script), *args],
            capture_output=True, text=True, check=False)

    def test_cli_no_baseline_skips(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "run.json"
            baseline = Path(tmp) / "baseline.json"
            self._write(run, _run())
            proc = self._cli("--run", str(run), "--baseline", str(baseline))
            self.assertEqual(proc.returncode, 0)
            self.assertIn("no baseline", proc.stderr)

    def test_cli_no_baseline_required_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "run.json"
            baseline = Path(tmp) / "baseline.json"
            self._write(run, _run())
            proc = self._cli("--run", str(run), "--baseline", str(baseline),
                             "--require-baseline")
            self.assertEqual(proc.returncode, 1)

    def test_cli_regression_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base_run = _run()
            baseline = Path(tmp) / "baseline.json"
            run = Path(tmp) / "run.json"
            self._write(baseline, _baseline(base_run))
            self._write(run, _run(scenarios={
                "cpu": _scenario(_metrics(e2e_p95_ns=9_999))}))
            proc = self._cli("--run", str(run), "--baseline", str(baseline))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("FAILED", proc.stderr)

    def test_cli_write_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "run.json"
            baseline = Path(tmp) / "baseline.json"
            self._write(run, _run())
            proc = self._cli("--run", str(run), "--baseline", str(baseline),
                             "--write-baseline")
            self.assertEqual(proc.returncode, 0)
            self.assertTrue(baseline.is_file())


if __name__ == "__main__":
    unittest.main()
