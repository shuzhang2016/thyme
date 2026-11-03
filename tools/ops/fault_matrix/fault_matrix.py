#!/usr/bin/env python3
"""OPS-08: fault-matrix driver.

Runs a bounded (< 60 s) matrix of role-kill and resource-exhaustion cases and
prints a JSON summary. Every case asserts a specific observable (state, exit
code, counter), never merely "did not crash".

Cases:
  kill_producer     real SIGKILL of a PinnedSlot producer; a reader must see the
                    committed prefix intact, and a new producer must be able to
                    recreate the channel.               [real kill -9]
  kill_reader       real SIGKILL of the recorder/consumer; the producer must
                    keep committing with no reader (no backpressure), and a
                    fresh reader must attach.            [real kill -9]
  kill_supervisor   real SIGKILL of the parent process; its role child is
                    armed with PR_SET_PDEATHSIG and must disappear. [real]
  shm_full          SHM create failure: the segment ftruncate is bounded with
                    RLIMIT_FSIZE (a full tmpfs, scaled down). Create must fail,
                    leave no object behind, and succeed once the limit is
                    restored.                [simulated: RLIMIT_FSIZE, no root]
  disk_full         ENOSPC drain failure: the recorder's documented
                    Config::fault_inject_write seam. The package must be
                    renamed .incomplete, disk_full set and disk_errors >= 1.
                            [simulated: injection seam, no tmpfs/loop mount]
  clock_jump        A clock jump at the trigger boundary: an out-of-window t0
                    in both directions must be rejected (kInvalidTimestamp)
                    and counted, an in-window trigger accepted.
     [simulated: caller timestamps at the control-plane; there is no injectable
      clock source in the runtime — CLOCK_MONOTONIC cannot be moved by a
      non-root process, and thyme reads it directly]

The driver cleans up every child, temp directory and /dev/shm object it
creates, and reports residual `thyme_fault_*` SHM objects in the summary.

The kill cases exercise the same fork+SIGKILL paths as `test_crash_injection`
and `test_replay_producer_gate`, and the ENOSPC case uses the same
`fault_inject_write` hook as `test_retention`; those in-process tests stay in
the default suite. The matrix adds the orchestrated, observable-level run
(exit signal, counter, package state) plus the resource-expiry cases they do
not cover.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SIMULATED = ("shm_full", "disk_full", "clock_jump")
SHM_PREFIX = "thyme_fault_"

# The ctest entry runs with THYME_SHM_TRACK=1 (test-only leak cleanup). Roles
# are real producers whose SHM must outlive their own exit, so they opt out.
CHILD_ENV = dict(os.environ)
CHILD_ENV["THYME_SHM_TRACK"] = "0"


class Child:
    """A helper process with a line-oriented JSON reader thread."""

    def __init__(self, argv: list[str], env: dict[str, str] | None = None):
        self.argv = argv
        self.proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env if env is not None else CHILD_ENV,
        )
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.stderr: list[str] = []
        threading.Thread(target=self._pump, args=(self.proc.stdout,),
                         daemon=True).start()
        threading.Thread(target=self._drain_stderr, daemon=True).start()

    def _pump(self, stream) -> None:
        try:
            for line in stream:
                self.lines.put(line.rstrip("\n"))
        finally:
            self.lines.put(None)

    def _drain_stderr(self) -> None:
        for line in self.proc.stderr:
            self.stderr.append(line.rstrip("\n"))

    def next_json(self, timeout: float) -> dict | None:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                return None
            if line is None:
                return None
            line = line.strip()
            if line.startswith("{"):
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    continue

    def wait_json(self, predicate, timeout: float) -> dict | None:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            value = self.next_json(remaining)
            if value is None:
                return None
            if predicate(value):
                return value

    def kill(self) -> int:
        if self.proc.poll() is None:
            self.proc.kill()
        return self.proc.wait(timeout=5)

    def terminate_and_wait(self) -> int:
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            return self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return self.proc.wait(timeout=5)


class Matrix:
    def __init__(self, helper: pathlib.Path, verbose: bool):
        self.helper = helper
        self.verbose = verbose
        self.children: list[Child] = []
        self.shm_names: list[str] = []
        self.temp_dirs: list[str] = []

    def spawn(self, *args: str) -> Child:
        child = Child([str(self.helper), *args])
        self.children.append(child)
        if self.verbose:
            print(f"spawn: {child.argv}", file=sys.stderr)
        return child

    def run(self, *args: str, timeout: float = 10.0) -> tuple[int, dict | None]:
        child = self.spawn(*args)
        try:
            rc = child.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            rc = child.kill()
        return rc, self.final_json(child)

    def final_json(self, child: Child, timeout: float = 1.5) -> dict | None:
        """Last JSON object printed by a child (None if it printed none).

        Waits briefly for the pump thread: process exit and pipe EOF are not
        the same instant, so an immediate poll can miss the final line.
        """
        last: dict | None = None
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                line = child.lines.get(timeout=remaining)
            except queue.Empty:
                break
            if line is None:
                break
            line = line.strip()
            if line.startswith("{"):
                try:
                    last = json.loads(line)
                except json.JSONDecodeError:
                    continue
        return last

    def unlink(self, name: str) -> None:
        if name not in self.shm_names:
            self.shm_names.append(name)
        self.run("unlink", name, timeout=5.0)

    def cleanup(self) -> list[str]:
        for child in self.children:
            if child.proc.poll() is None:
                child.kill()
        for name in self.shm_names:
            path = pathlib.Path("/dev/shm") / name.lstrip("/")
            try:
                path.unlink()
            except FileNotFoundError:
                pass
            lease = pathlib.Path(str(path) + ".lease")
            try:
                lease.unlink()
            except FileNotFoundError:
                pass
        for directory in self.temp_dirs:
            shutil.rmtree(directory, ignore_errors=True)
        residual: list[str] = []
        shm = pathlib.Path("/dev/shm")
        try:
            for entry in shm.iterdir():
                if entry.name.startswith(SHM_PREFIX):
                    residual.append(entry.name)
        except OSError:
            pass
        return residual


def case_kill_producer(m: Matrix) -> tuple[bool, dict]:
    name = f"/{SHM_PREFIX}prod_{os.getpid()}"
    m.shm_names.append(name)
    m.unlink(name)

    producer = m.spawn("producer", name, "2048", "64", "0")
    seen = producer.wait_json(
        lambda j: int(j.get("committed", 0)) >= 16, timeout=5.0)
    committed = int(seen.get("committed", 0)) if seen else 0
    rc = producer.kill()
    observables = {
        "committed_before_kill": committed,
        "exit_signal": -rc,
    }
    ok = committed >= 16 and rc == -signal.SIGKILL

    rc, report = m.run("reader", name, "1", "drain", timeout=5.0)
    report = report or {}
    frames = int(report.get("frames", 0))
    head = int(report.get("head", 0))
    observables.update({
        "reader_exit": rc,
        "frames_after_kill": frames,
        "head_after_kill": head,
        "torn": report.get("torn"),
        "bad": report.get("bad"),
        "drops": report.get("drops"),
    })
    # Strong invariant: every committed frame was either read intact or counted
    # as an overrun drop; nothing torn and nothing partially visible.
    ok = ok and rc == 0 and frames >= 1 and head >= frames
    ok = ok and report.get("torn") == 0 and report.get("bad") == 0
    ok = ok and frames + int(report.get("drops", 0)) == head

    # A new producer must be able to recreate the (dead) channel and publish.
    rc_new, new_report = m.run("producer", name, "2048", "64", "8", timeout=5.0)
    new_report = new_report or {}
    observables["recreate_exit"] = rc_new
    observables["recreate_committed"] = new_report.get("committed")
    ok = ok and rc_new == 0 and int(new_report.get("committed", 0)) >= 8
    rc_reader, rereport = m.run("reader", name, "8", "drain", timeout=5.0)
    rereport = rereport or {}
    observables["recreate_frames"] = rereport.get("frames")
    ok = ok and rc_reader == 0 and int(rereport.get("frames", 0)) >= 8
    return ok, observables


def case_kill_reader(m: Matrix) -> tuple[bool, dict]:
    name = f"/{SHM_PREFIX}reader_{os.getpid()}"
    m.shm_names.append(name)
    m.unlink(name)

    producer = m.spawn("producer", name, "2048", "256", "0")
    first = producer.wait_json(
        lambda j: int(j.get("committed", 0)) >= 8, timeout=5.0)
    if first is None:
        producer.kill()
        return False, {"error": "producer did not start"}

    follower = m.spawn("reader", name, "1", "follow")
    attached = follower.wait_json(lambda j: j.get("attached") is True,
                                  timeout=5.0)
    rc = follower.kill()

    # The producer must keep committing after the consumer is gone.
    before = int(first.get("committed", 0))
    after_msg = producer.wait_json(
        lambda j: int(j.get("committed", 0)) > before, timeout=5.0)
    after = int(after_msg.get("committed", 0)) if after_msg else before
    producer.kill()

    observables = {
        "consumer_attached": attached is not None,
        "consumer_exit_signal": -rc,
        "committed_before_kill": before,
        "committed_after_kill": after,
        "producer_kept_committing": after > before,
    }
    ok = attached is not None and rc == -signal.SIGKILL and after > before

    rc_new, report = m.run("reader", name, "1", "drain", timeout=5.0)
    report = report or {}
    observables["fresh_reader_exit"] = rc_new
    observables["fresh_reader_frames"] = report.get("frames")
    ok = ok and rc_new == 0 and int(report.get("frames", 0)) >= 1
    ok = ok and report.get("torn") == 0 and report.get("bad") == 0
    return ok, observables


def case_kill_supervisor(m: Matrix) -> tuple[bool, dict]:
    parent = m.spawn("pdeathsig-parent")
    ready = parent.wait_json(lambda j: "child" in j, timeout=5.0)
    if ready is None:
        parent.kill()
        return False, {"error": "pdeathsig parent did not report a child"}
    child_pid = int(ready["child"])
    rc = parent.kill()
    deadline = time.monotonic() + 3.0
    gone_at = None
    while time.monotonic() < deadline:
        if not pathlib.Path(f"/proc/{child_pid}").exists():
            gone_at = round(3.0 - (deadline - time.monotonic()), 3)
            break
        time.sleep(0.01)
    observables = {
        "parent_exit_signal": -rc,
        "child_pid": child_pid,
        "child_gone": gone_at is not None,
        "child_gone_after_s": gone_at,
    }
    ok = rc == -signal.SIGKILL and gone_at is not None
    if gone_at is None:
        try:
            os.kill(child_pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return ok, observables


def case_shm_full(m: Matrix) -> tuple[bool, dict]:
    name = f"/{SHM_PREFIX}shmfull_{os.getpid()}"
    m.shm_names.append(name)
    rc, report = m.run("shm-full", name, str(8 * 1024 * 1024), timeout=10.0)
    report = report or {}
    observables = {
        "exit": rc,
        "limit_set": report.get("limit_set"),
        "create_failed": report.get("create_failed"),
        "residue": report.get("residue"),
        "retry_ok": report.get("retry_ok"),
    }
    ok = (rc == 0 and report.get("ok") is True
          and report.get("create_failed") is True
          and report.get("residue") is False
          and report.get("retry_ok") is True)
    return ok, observables


def case_disk_full(m: Matrix) -> tuple[bool, dict]:
    out_dir = tempfile.mkdtemp(prefix="thyme_fault_disk_")
    m.temp_dirs.append(out_dir)
    mirror = f"/{SHM_PREFIX}mirror_{os.getpid()}"
    m.shm_names.append(mirror)
    rc, report = m.run("recorder-enospc", out_dir, mirror, timeout=10.0)
    report = report or {}
    packages = sorted(pathlib.Path(out_dir).glob("*.incomplete"))
    observables = {
        "exit": rc,
        "disk_full": report.get("disk_full"),
        "disk_errors": report.get("disk_errors"),
        "incomplete_package": bool(packages),
        "sealed": report.get("sealed"),
        "package": packages[0].name if packages else None,
    }
    ok = (rc == 0 and report.get("ok") is True
          and report.get("disk_full") is True
          and int(report.get("disk_errors", 0)) >= 1
          and report.get("sealed") is False
          and bool(packages))
    return ok, observables


def case_clock_jump(m: Matrix) -> tuple[bool, dict]:
    rc, report = m.run("clock-jump", timeout=10.0)
    report = report or {}
    observables = {
        "exit": rc,
        "backward": report.get("backward"),
        "forward": report.get("forward"),
        "in_window": report.get("in_window"),
        "rejected_bad_timestamp": report.get("rejected_bad_timestamp"),
    }
    ok = (rc == 0 and report.get("ok") is True
          and report.get("backward") == "kInvalidTimestamp"
          and report.get("forward") == "kInvalidTimestamp"
          and report.get("in_window") == "kAccepted"
          and int(report.get("rejected_bad_timestamp", 0)) == 2)
    return ok, observables


CASES = (
    ("kill_producer", case_kill_producer, "real kill -9 (producer role)"),
    ("kill_reader", case_kill_reader, "real kill -9 (recorder/consumer role)"),
    ("kill_supervisor", case_kill_supervisor,
     "real kill -9 (supervisor); child exits via PR_SET_PDEATHSIG"),
    ("shm_full", case_shm_full,
     "simulated: RLIMIT_FSIZE bounds the tmpfs segment (no root)"),
    ("disk_full", case_disk_full,
     "simulated: Config::fault_inject_write ENOSPC seam (no tmpfs/loop mount)"),
    ("clock_jump", case_clock_jump,
     "simulated: caller t0 at the trigger boundary; no injectable clock "
     "source, CLOCK_MONOTONIC is not settable"),
)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="thyme OPS-08 fault matrix")
    parser.add_argument("--helper", required=True,
                        help="fault_role binary built by tools/ops/fault_matrix")
    parser.add_argument("--json", help="write the JSON summary to this path")
    parser.add_argument("--only", action="append", default=[],
                        help="run only the named case (repeatable)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    helper = pathlib.Path(args.helper).resolve()
    if not helper.is_file():
        print(f"fault_matrix: helper not found: {helper}", file=sys.stderr)
        return 2

    selected = [c for c in CASES if not args.only or c[0] in args.only]
    if not selected:
        print(f"fault_matrix: no such case: {', '.join(args.only)}",
              file=sys.stderr)
        return 2

    matrix = Matrix(helper, args.verbose)
    started = time.monotonic()
    results = []
    for name, function, detail in selected:
        case_start = time.monotonic()
        try:
            ok, observables = function(matrix)
        except Exception as error:  # noqa: BLE001 - report, never abort the run
            ok, observables = False, {"exception": f"{type(error).__name__}: {error}"}
        results.append({
            "name": name,
            "kind": "simulated" if name in SIMULATED else "real",
            "detail": detail,
            "ok": bool(ok),
            "duration_ms": round((time.monotonic() - case_start) * 1000, 1),
            "observables": observables,
        })
        if args.verbose:
            print(f"case {name}: {'ok' if ok else 'FAILED'}", file=sys.stderr)

    residual = matrix.cleanup()
    summary = {
        "tool": "fault_matrix",
        "case": "OPS-08",
        "ok": all(case["ok"] for case in results) and not residual,
        "duration_ms": round((time.monotonic() - started) * 1000, 1),
        "helper": str(helper),
        "simulated": [name for name, _, _ in selected if name in SIMULATED],
        "cases": results,
        "residual_shm": residual,
        "cleanup_ok": not residual,
    }
    text = json.dumps(summary, indent=2, sort_keys=False)
    print(text)
    if args.json:
        path = pathlib.Path(args.json)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text + "\n", encoding="utf-8")
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
