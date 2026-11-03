#!/usr/bin/env python3
from __future__ import annotations

import contextlib
import io
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_test_environment as preflight  # noqa: E402


class EnvironmentPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        self.proc = self.root / "proc"
        self.shm = self.root / "shm"
        self.proc.mkdir()
        self.shm.mkdir()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def process(self, pid: int, ppid: int, comm: str) -> None:
        directory = self.proc / str(pid)
        directory.mkdir()
        (directory / "stat").write_text(
            f"{pid} ({comm}) S {ppid} 0 0 0\n", encoding="utf-8")

    def test_detects_only_orphan_workloads(self) -> None:
        self.process(10, 1, "yes")
        self.process(11, 1, "test_stress")
        self.process(12, 2, "test_active")
        self.process(13, 1, "demo_producer")
        self.process(14, 1, "trigger_center")
        self.assertEqual(preflight.orphan_processes(self.proc),
                         [(10, "yes"), (11, "test_stress"),
                          (13, "demo_producer")])

    def test_missing_process_is_ignored(self) -> None:
        (self.proc / "not-a-pid").mkdir()
        (self.proc / "14").mkdir()
        self.assertEqual(preflight.orphan_processes(self.proc), [])

    def test_shm_stats_count_objects_and_free_bytes(self) -> None:
        (self.shm / "one").write_bytes(b"1")
        (self.shm / "two").write_bytes(b"2")
        count, free_bytes = preflight.shm_stats(self.shm)
        self.assertEqual(count, 2)
        self.assertGreater(free_bytes, 0)

    def test_main_fails_on_orphan_without_deleting_shm(self) -> None:
        self.process(10, 1, "yes")
        (self.shm / "keep").write_bytes(b"payload")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = preflight.main(["--proc-root", str(self.proc),
                                     "--shm-dir", str(self.shm)])
        self.assertEqual(result, 1)
        self.assertTrue((self.shm / "keep").exists())


class TestLauncherTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.shm = pathlib.Path(self._tmp.name)
        self.launcher = pathlib.Path(__file__).with_name("test_launcher.py")
        self.env = dict(os.environ)
        self.env["THYME_TEST_SHM_DIR"] = str(self.shm)
        self.env["THYME_SHM_TEST_PREFIX"] = "thyme_launcher_test_"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_launcher(self, code: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(self.launcher), sys.executable, "-c", code],
            env=self.env, capture_output=True, text=True, timeout=10)

    def test_exit_code_and_cleanup_are_scoped(self) -> None:
        (self.shm / "unrelated").write_text("keep")
        code = (
            "import os,pathlib;"
            "p=pathlib.Path(os.environ['THYME_TEST_SHM_DIR']);"
            "(p/(os.environ['THYME_SHM_TEST_PREFIX']+'sample')).write_text('x');"
            "raise SystemExit(7)")
        result = self.run_launcher(code)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual([p.name for p in self.shm.iterdir()], ["unrelated"])

    def test_orphan_group_and_pid_name_are_reaped(self) -> None:
        code = (
            "import os,pathlib,subprocess;"
            "p=pathlib.Path(os.environ['THYME_TEST_SHM_DIR']);"
            "c=subprocess.Popen(['sleep','60']);"
            "(p/('orphan_'+str(c.pid))).write_text('x');"
            "print(c.pid,flush=True)")
        result = self.run_launcher(code)
        self.assertEqual(result.returncode, 0, result.stderr)
        orphan = int(result.stdout.strip())
        self.assertFalse(pathlib.Path(f"/proc/{orphan}").exists())
        self.assertEqual(list(self.shm.iterdir()), [])

    def test_sigterm_cleans_long_running_child(self) -> None:
        code = (
            "import os,pathlib,time;"
            "p=pathlib.Path(os.environ['THYME_TEST_SHM_DIR']);"
            "(p/(os.environ['THYME_SHM_TEST_PREFIX']+'wait')).write_text('x');"
            "time.sleep(60)")
        proc = subprocess.Popen(
            [sys.executable, str(self.launcher), sys.executable, "-c", code],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)
        expected = None
        deadline = time.monotonic() + 5
        while expected is None and time.monotonic() < deadline:
            candidates = list(self.shm.glob("*wait"))
            if candidates:
                expected = candidates[0]
            else:
                time.sleep(0.02)
        self.assertIsNotNone(expected)
        proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 128 + signal.SIGTERM, stderr)
        self.assertFalse(expected.exists())
        self.assertEqual(stdout, "")


if __name__ == "__main__":
    unittest.main()
