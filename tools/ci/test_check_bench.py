#!/usr/bin/env python3
"""Unit tests for tools/ci/check_bench.py (PERF-06).

Plain ``unittest`` with in-memory fixture JSON. Registered as
``test_check_bench`` (labels ``unit;mr_gate``) in the root CMakeLists. The
wall-clock benches themselves run only in the scheduled ``nightly-bench`` job;
MRs gate the comparison logic and the CI job shape.
"""

from __future__ import annotations

import copy
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_bench as cb  # noqa: E402

TAG = "x86_64-local"
HOST = {"hostname": "host-a", "machine": "x86_64"}
ROOT = Path(__file__).resolve().parents[2]


def _metric(ns: float, p99: float | None = 20.0,
            p999: float | None = 30.0, iterations: int = 100) -> dict:
    item = {"name": "m1", "ns_per_op": ns, "iterations": iterations,
            "bytes_per_op": 0, "gbps": None}
    if p99 is not None:
        item["p99_ns_per_op"] = p99
    if p999 is not None:
        item["p999_ns_per_op"] = p999
    return item


def _core(metrics: dict[str, dict] | None = None,
          host: dict | None = None, tag: str = TAG,
          ok: bool = True) -> dict:
    if metrics is None:
        metrics = {"m1": _metric(10.0)}
    benchmarks = []
    for name, metric in metrics.items():
        item = copy.deepcopy(metric)
        item["name"] = name
        benchmarks.append(item)
    return {
        "kind": cb.CORE_KIND,
        "schema": 1,
        "runner_tag": tag,
        "hostname": (host or HOST)["hostname"],
        "host": dict(host or HOST),
        "git_sha": "abc1234",
        "toolchain": "GCC 11.4.0",
        "build_type": "Release",
        "ok": ok,
        "benchmarks": benchmarks,
    }


def _object_pool(metrics: dict[str, dict] | None = None,
                 host: dict | None = None, tag: str = TAG) -> dict:
    doc = _core(metrics, host, tag)
    doc["kind"] = cb.OBJECT_POOL_KIND
    return doc


def _seal(p50: float = 1000.0, p99: float = 2000.0,
          p999: float = 3000.0, host: dict | None = None,
          tag: str = TAG) -> dict:
    return {
        "kind": cb.TRIGGER_SEAL_KIND,
        "schema": 1,
        "runner_tag": tag,
        "hostname": (host or HOST)["hostname"],
        "host": dict(host or HOST),
        "ok": True,
        "trigger_to_seal_ns": {
            "n": 64, "min": 900, "p50": p50, "p90": p50,
            "p99": p99, "p999": p999, "max": p999, "mean": p50,
        },
    }


def _entry(run: dict, host: dict | None = None) -> dict:
    doc = cb.normalize_run(run, Path("<run>"))
    return {
        "recorded_at": "2026-10-24T00:00:00Z",
        "git_sha": "abc1234",
        "build_type": "Release",
        "host": dict(host or doc["host"]),
        "benchmarks": {
            name: {
                "bench": doc["kind"],
                "ns_per_op": metric["ns_per_op"],
                "p99_ns_per_op": metric["p99_ns_per_op"],
                "p999_ns_per_op": metric["p999_ns_per_op"],
                "bytes_per_op": metric["bytes_per_op"],
                "iterations": metric["iterations"],
                "gbps": metric["gbps"],
            }
            for name, metric in doc["metrics"].items()
        },
    }


def _baseline(*runs: dict, host: dict | None = None) -> dict:
    return {
        "schema": 2,
        "tolerance_ns_per_op_pct": 25.0,
        "tolerance_p99_pct": 50.0,
        "tolerance_p999_pct": 100.0,
        "runners": {
            (runs[0] if runs else _core())["runner_tag"]: _entry(
                runs[0] if runs else _core(), host),
        },
    }


def _gate(runs: list[dict], baseline: dict, *,
          ns: float | None = None, p99: float | None = None,
          p999: float | None = None,
          require_host: bool = False) -> dict:
    docs = [cb.normalize_run(run, Path("<run>")) for run in runs]
    tolerances = cb.tolerance_set(baseline, ns, p99, p999)
    return cb.compare_documents(docs, baseline, tolerances, require_host)


class MetricTests(unittest.TestCase):
    def test_within_all_tolerances(self) -> None:
        run = _core({"m1": _metric(12.0, 29.0, 59.0)})  # +20/+45/+97
        result = _gate([run], _baseline())
        self.assertEqual(result["status"], "pass")
        row = result["rows"][0]
        self.assertEqual(row["status"], "pass")
        self.assertEqual(row["p99_status"], "pass")
        self.assertEqual(row["p999_status"], "pass")

    def test_ns_boundary(self) -> None:
        passing = _core({"m1": _metric(12.5)})   # exactly +25%
        failing = _core({"m1": _metric(12.6)})
        self.assertEqual(_gate([passing], _baseline())["status"], "pass")
        result = _gate([failing], _baseline())
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["rows"][0]["status"], "fail")

    def test_p99_tolerance(self) -> None:
        passing = _core({"m1": _metric(10.0, p99=30.0)})  # +50%
        failing = _core({"m1": _metric(10.0, p99=33.0)})  # +65%
        self.assertEqual(_gate([passing], _baseline())["status"], "pass")
        result = _gate([failing], _baseline())
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["rows"][0]["p99_status"], "fail")

    def test_p999_tolerance(self) -> None:
        passing = _core({"m1": _metric(10.0, p999=60.0)})  # +100%
        failing = _core({"m1": _metric(10.0, p999=61.0)})  # +103%
        self.assertEqual(_gate([passing], _baseline())["status"], "pass")
        result = _gate([failing], _baseline())
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["rows"][0]["p999_status"], "fail")

    def test_missing_metric_fails(self) -> None:
        baseline = _baseline(_core({"m1": _metric(10.0), "m2": _metric(5.0)}))
        result = _gate([_core({"m1": _metric(10.0)})], baseline)
        self.assertEqual(result["status"], "fail")
        self.assertIn("m2", [r["metric"] for r in result["rows"]
                             if r["status"] == "missing"])

    def test_missing_percentile_fails(self) -> None:
        run = _core({"m1": _metric(10.0, p99=None)})
        result = _gate([run], _baseline())
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["rows"][0]["p99_status"], "missing")

    def test_new_metric_is_not_gated(self) -> None:
        run = _core({"m1": _metric(10.0), "m2": _metric(999.0)})
        result = _gate([run], _baseline())
        self.assertEqual(result["status"], "pass")
        self.assertEqual(
            [r["status"] for r in result["rows"] if r["metric"] == "m2"],
            ["new"])

    def test_new_percentile_is_not_gated(self) -> None:
        baseline = _baseline(_core({"m1": _metric(10.0, p99=None,
                                                  p999=None)}))
        run = _core({"m1": _metric(10.0, p99=99.0, p999=999.0)})
        result = _gate([run], baseline)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["rows"][0]["p99_status"], "new")

    def test_tolerance_override(self) -> None:
        run = _core({"m1": _metric(12.6)})  # +26%
        self.assertEqual(_gate([run], _baseline(), ns=30.0)["status"], "pass")

    def test_failed_correctness_run(self) -> None:
        result = _gate([_core(ok=False)], _baseline())
        self.assertEqual(result["status"], "fail")
        self.assertIn("correctness", result["note"])


class DocumentTests(unittest.TestCase):
    def test_trigger_seal_flattens_to_distribution(self) -> None:
        doc = cb.normalize_run(_seal(), Path("<run>"))
        self.assertEqual(list(doc["metrics"]), [cb.SEAL_METRIC])
        metric = doc["metrics"][cb.SEAL_METRIC]
        self.assertEqual(metric["ns_per_op"], 1000.0)
        self.assertEqual(metric["p99_ns_per_op"], 2000.0)
        self.assertEqual(metric["p999_ns_per_op"], 3000.0)
        self.assertEqual(metric["iterations"], 64)

    def test_trigger_seal_gate(self) -> None:
        run = _seal(p50=1100.0, p99=2500.0, p999=5000.0)
        baseline = _baseline(_seal())
        result = _gate([run], baseline)
        self.assertEqual(result["status"], "pass")
        result = _gate([_seal(p50=1400.0)], baseline)
        self.assertEqual(result["status"], "fail")

    def test_subset_run_ignores_other_kinds(self) -> None:
        core = _core({"m1": _metric(10.0)})
        pool = _object_pool({"object_pool_dense": _metric(5.0)})
        baseline = {
            "runners": {
                TAG: {
                    "host": dict(HOST),
                    "benchmarks": {
                        **copy.deepcopy(_entry(core)["benchmarks"]),
                        **copy.deepcopy(_entry(pool)["benchmarks"]),
                    },
                }
            }
        }
        result = _gate([core], baseline)
        self.assertEqual(result["status"], "pass")
        self.assertEqual([row["metric"] for row in result["rows"]], ["m1"])

    def test_unknown_kind_rejected(self) -> None:
        run = _core()
        run["kind"] = "something-else"
        with self.assertRaises(SystemExit):
            cb.normalize_run(run, Path("<run>"))

    def test_missing_runner_tag_rejected(self) -> None:
        run = _core()
        del run["runner_tag"]
        with self.assertRaises(SystemExit):
            cb.normalize_run(run, Path("<run>"))

    def test_empty_benchmarks_rejected(self) -> None:
        run = _core()
        run["benchmarks"] = []
        with self.assertRaises(SystemExit):
            cb.normalize_run(run, Path("<run>"))

    def test_mixed_tags_across_runs_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            _gate([_core(), _object_pool(tag="x86_64-ci")], _baseline())


class HostTests(unittest.TestCase):
    def test_hostname_mismatch_skips(self) -> None:
        run = _core(host={"hostname": "host-b", "machine": "x86_64"})
        result = _gate([run], _baseline())
        self.assertEqual(result["status"], "host-mismatch")
        self.assertIn("hostname", result["note"])

    def test_arch_mismatch_skips(self) -> None:
        run = _core(host={"hostname": "host-a", "machine": "aarch64"})
        result = _gate([run], _baseline())
        self.assertEqual(result["status"], "host-mismatch")
        self.assertIn("arch", result["note"])

    def test_mismatch_can_be_required(self) -> None:
        run = _core(host={"hostname": "host-b", "machine": "x86_64"})
        result = _gate([run], _baseline(), require_host=True)
        self.assertEqual(result["status"], "fail")

    def test_correctness_failure_wins_over_host_mismatch(self) -> None:
        run = _core(host={"hostname": "host-b", "machine": "x86_64"},
                    ok=False)
        result = _gate([run], _baseline())
        self.assertEqual(result["status"], "fail")

    def test_legacy_baseline_hostname_fallback(self) -> None:
        baseline = _baseline()
        entry = baseline["runners"][TAG]
        entry["hostname"] = entry["host"]["hostname"]
        del entry["host"]
        result = _gate([_core()], baseline)
        self.assertEqual(result["status"], "pass")
        result = _gate([_core(host={"hostname": "host-b",
                                    "machine": "x86_64"})], baseline)
        self.assertEqual(result["status"], "host-mismatch")


class NoBaselineTests(unittest.TestCase):
    def test_no_baseline_for_runner(self) -> None:
        result = _gate([_core(tag="x86_64-ci")], _baseline())
        self.assertEqual(result["status"], "no-baseline")
        self.assertTrue(all(row["status"] == "new" for row in result["rows"]))

    def test_no_baseline_required_fails_via_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "run.json"
            baseline = Path(tmp) / "baseline.json"
            run.write_text(json.dumps(_core(tag="x86_64-ci")),
                           encoding="utf-8")
            proc = _cli("--run", str(run), "--baseline", str(baseline),
                        "--require-baseline")
            self.assertEqual(proc.returncode, 1)


class WriteBaselineTests(unittest.TestCase):
    def test_merges_kinds_and_records_host(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "baseline.json"
            cb.write_baseline(path, [_core()], None)
            cb.write_baseline(path, [_object_pool({"object_pool_dense":
                                                   _metric(5.0)})],
                              json.loads(path.read_text(encoding="utf-8")))
            doc = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(doc["schema"], 2)
            self.assertIn("x86_64-ci", doc["unset_runners"])
            entry = doc["runners"][TAG]
            self.assertEqual(entry["role"], "reference-only")
            self.assertEqual(entry["host"], HOST)
            self.assertEqual(entry["benchmarks"]["m1"]["bench"],
                             cb.CORE_KIND)
            self.assertEqual(
                entry["benchmarks"]["object_pool_dense"]["bench"],
                cb.OBJECT_POOL_KIND)
            self.assertEqual(
                doc["tolerance_p99_pct"], cb.DEFAULT_P99_TOLERANCE_PCT)

    def test_refuses_failed_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "baseline.json"
            with self.assertRaises(SystemExit):
                cb.write_baseline(path, [_core(ok=False)], None)

    def test_refuses_mixed_tags(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "baseline.json"
            with self.assertRaises(SystemExit):
                cb.write_baseline(
                    path, [_core(), _object_pool(tag="x86_64-ci")], None)


class CliTests(unittest.TestCase):
    def test_no_baseline_skips(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "run.json"
            baseline = Path(tmp) / "baseline.json"
            run.write_text(json.dumps(_core()), encoding="utf-8")
            proc = _cli("--run", str(run), "--baseline", str(baseline))
            self.assertEqual(proc.returncode, 0)
            self.assertIn("no baseline", proc.stderr)

    def test_regression_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "run.json"
            baseline = Path(tmp) / "baseline.json"
            run.write_text(json.dumps(_core({"m1": _metric(99.0)})),
                           encoding="utf-8")
            baseline.write_text(json.dumps(_baseline()), encoding="utf-8")
            proc = _cli("--run", str(run), "--baseline", str(baseline))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("FAILED", proc.stderr)

    def test_write_baseline_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "run.json"
            baseline = Path(tmp) / "baseline.json"
            run.write_text(json.dumps(_core()), encoding="utf-8")
            proc = _cli("--run", str(run), "--baseline", str(baseline),
                        "--write-baseline")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertTrue(baseline.is_file())


def _cli(*args: str) -> subprocess.CompletedProcess[str]:
    script = Path(__file__).resolve().parent / "check_bench.py"
    return subprocess.run(
        [sys.executable, str(script), *args],
        capture_output=True, text=True, check=False)


class CiJobTests(unittest.TestCase):
    """The scheduled bench job must exist and must never be PR-blocking."""

    @classmethod
    def setUpClass(cls) -> None:
        try:
            import yaml  # type: ignore
        except ImportError:  # pragma: no cover - CI installs python3-yaml
            raise unittest.SkipTest("PyYAML not installed")
        cls.pipeline = yaml.safe_load(
            (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))

    @staticmethod
    def _job_script(job: dict) -> str:
        return "\n".join(step.get("run", "") for step in job.get("steps", []))

    def test_nightly_bench_shape(self) -> None:
        job = self.pipeline["jobs"]["nightly-bench"]
        self.assertIsNot(job.get("continue-on-error"), True)
        script = self._job_script(job)
        for artifact in ("bench_core.json", "bench_object_pool.json",
                         "bench_trigger_seal.json"):
            self.assertIn(artifact, script)
        self.assertIn("check_bench.py", script)
        self.assertIn("--tolerance-p99", script)
        self.assertIn("--tolerance-p999", script)
        self.assertEqual(job["env"]["THYME_BENCH_RUNNER_TAG"], "x86_64-ci")

    def test_nightly_bench_is_not_mr_blocking(self) -> None:
        condition = self.pipeline["jobs"]["nightly-bench"].get("if", "")
        self.assertNotIn("pull_request", condition)
        self.assertIn("schedule", condition)
        self.assertIn("workflow_dispatch", condition)

    def test_mr_gate_does_not_run_wall_clock_benches(self) -> None:
        script = self._job_script(self.pipeline["jobs"]["test-mr-gate"])
        self.assertNotIn("check_bench.py", script)
        self.assertNotIn("bench_core --json", script)
        self.assertIn("-L mr_gate", script)


if __name__ == "__main__":
    unittest.main()
