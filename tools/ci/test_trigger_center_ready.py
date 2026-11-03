#!/usr/bin/env python3
"""OPS-11: end-to-end readiness handshake for `trigger_center`.

The unit claims three things a textual check cannot prove:

  * `ExecStartPre`'s `--check-config` and `--check-resources` both succeed on a
    deployable config;
  * the process sends `READY=1` only after the recorder attached its sources
    and both sockets are bound;
  * the main loop feeds `WATCHDOG=1`, and a planned SIGTERM sends `STOPPING=1`
    before a clean exit.

This test builds that stack for real with `demo_producer` (producer pages) and
a private `NOTIFY_SOCKET` datagram socket. It is not a host-capacity test:
`THYME_RESOURCE_CHECK=warn` keeps a small CI container's /dev/shm out of the
way (OPS-07 has its own test). Every process and SHM object it creates is
named after its pid and removed in `finally`.

Usage:
  test_trigger_center_ready.py --trigger-center <path> --demo-producer <path>
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

READY_DEADLINE_S = 15.0
WATCHDOG_DEADLINE_S = 10.0
STOP_DEADLINE_S = 10.0
POLL_INTERVAL_S = 0.01


class Failure(RuntimeError):
    pass


def wait_for(pred, deadline_s: float, what: str):
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(POLL_INTERVAL_S)
    raise Failure(f"timed out after {deadline_s:.1f}s waiting for {what}")


class NotifySink:
    """Bound AF_UNIX datagram socket standing in for systemd's NOTIFY_SOCKET."""

    def __init__(self, path: pathlib.Path):
        self.path = path
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(str(path))
        self.sock.settimeout(0.1)

    def wait_for(self, state: str, deadline_s: float):
        seen: list[str] = []

        def poll():
            while True:
                try:
                    data = self.sock.recv(4096)
                except socket.timeout:
                    return None
                message = data.decode("utf-8", errors="replace").strip()
                seen.append(message)
                if message == state:
                    return message

        try:
            return wait_for(poll, deadline_s, f"NOTIFY_SOCKET {state!r}")
        except Failure as exc:
            raise Failure(f"{exc}; received {seen!r}") from exc

    def close(self):
        self.sock.close()
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


def write_config(path: pathlib.Path, tmp: pathlib.Path, tag: str) -> dict:
    """Writes a deployable config and returns the SHM names it references."""
    names = {
        "page": f"/thyme_tc_ready_{tag}_page",
        "ring": f"/thyme_tc_ready_{tag}_ring",
        "chan": f"/thyme_tc_ready_{tag}_chan",
    }
    out_dir = tmp / "out"
    out_dir.mkdir()
    path.write_text(
        f"""
[trigger_center]
socket = {tmp}/trigger.sock
max_concurrent_captures = 1
default_source = manual
crash_priority = 100

[logging]
service_file = {tmp}/service.log
service_max_bytes = 1048576
service_rotate_keep = 2

[recorder]
output_dir = {out_dir}
socket = {tmp}/recorder.sock
pending_bytes_quota = 67108864
flush_policy = idle
disk_quota_mb = 100

[channel.{names['page']}]
kind = mirror_ring
name = can
rate_hz = 100
avg_frame_bytes = 16

[channel.{names['chan']}]
kind = pinned_slot
name = cam

[source.manual]
channels = can, cam
before_s = 1
after_s = 1
priority = 10
mute_s = 0
quota_class = manual

[quota.manual]
max_triggers_per_hour = 10
""".lstrip(),
        encoding="utf-8",
    )
    return names


def shm_names(tag: str) -> list[pathlib.Path]:
    return [
        pathlib.Path("/dev/shm") / f"thyme_tc_ready_{tag}_{suffix}"
        for suffix in ("page", "ring", "chan")
    ]


def cleanup_shm(tag: str):
    prefix = f"thyme_tc_ready_{tag}_"
    shm = pathlib.Path("/dev/shm")
    if not shm.is_dir():
        return
    for path in shm.iterdir():
        if path.name.startswith(prefix):
            try:
                path.unlink()
            except FileNotFoundError:
                pass


def leftover_shm(tag: str) -> list[pathlib.Path]:
    prefix = f"thyme_tc_ready_{tag}_"
    shm = pathlib.Path("/dev/shm")
    if not shm.is_dir():
        return []
    return [path for path in shm.iterdir() if path.name.startswith(prefix)]


def _pdeathsig() -> None:
    """Kill the child if this (test) process dies, even by SIGKILL.

    ctest may SIGKILL a test that overruns its timeout; without this the
    spawned demo_producer keeps running forever and holds its SHM pages (a
    2h37m orphan was observed). Best-effort: the syscall is Linux-only.
    """
    try:
        libc = ctypes.CDLL(ctypes.util.find_library("c") or "libc.so.6",
                           use_errno=True)
        # PR_SET_PDEATHSIG = 1, SIGKILL = 9
        libc.prctl(1, signal.SIGKILL, 0, 0, 0)
    except Exception:
        pass


def stop_process(proc: subprocess.Popen, grace_s: float = 5.0) -> int:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
    try:
        return proc.wait(timeout=grace_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.wait(timeout=5.0)


def session_lease_shm_name(root: str) -> str:
    """Mirror of SessionLeaseNameForRoot (FNV-1a 64 of the out_dir)."""
    h = 14695981039346656037
    for byte in root.encode():
        h ^= byte
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    if h == 0:
        h = 1
    return f"/tsess_{h:016x}"


def unlink_session_lease(out_dir: pathlib.Path) -> None:
    """ARCH-14: the recorder keeps its lease page on purpose; a test with a
    pid-scoped out_dir must unlink it or check_shm_leaks reports a leak."""
    try:
        os.unlink(f"/dev/shm/{session_lease_shm_name(str(out_dir)).lstrip('/')}")
    except OSError:
        pass


def run(trigger_center: pathlib.Path, demo_producer: pathlib.Path) -> None:
    tag = str(os.getpid())
    tmp = pathlib.Path(tempfile.mkdtemp(prefix=f"thyme-tc-ready-{tag}-"))
    config = tmp / "trigger-center.ini"
    names = write_config(config, tmp, tag)
    manifest = tmp / "record_manifest.h"
    manifest.write_text(
        "inline constexpr unsigned kRecordManifestCount = 2;\n"
        "inline constexpr const char* kRecordManifestChannels[] = {\n"
        "  \"cam\",\n  \"can\"\n};\n",
        encoding="utf-8",
    )
    env = dict(os.environ)
    env["THYME_RESOURCE_CHECK"] = "warn"
    env["THYME_NO_MLOCK"] = "1"
    env["THYME_SD_NOTIFY"] = "1"
    # Daemons must keep their SHM until this script cleans up explicitly.
    env["THYME_SHM_TRACK"] = "0"

    producer = None
    center = None
    notify = None
    center_log = tmp / "center.log"
    producer_log = tmp / "producer.log"
    try:
        cleanup_shm(tag)  # a previous crashed run cannot poison this one
        with producer_log.open("wb") as plog:
            producer = subprocess.Popen(
                [str(demo_producer), names["page"], names["ring"],
                 names["chan"], "100"],
                stdout=plog, stderr=subprocess.STDOUT, env=env,
                preexec_fn=_pdeathsig)
        for shm_name in shm_names(tag):
            wait_for(lambda p=shm_name: p.exists(), READY_DEADLINE_S,
                     f"producer SHM {shm_name}")

        # The unit's ExecStartPre gates run against this exact config.
        for flag in ("--check-config", "--check-resources"):
            proc = subprocess.run(
                [str(trigger_center), flag, "--record-manifest",
                 str(manifest), str(config)],
                capture_output=True, text=True, env=env, check=False)
            if proc.returncode != 0:
                raise Failure(
                    f"{flag} exited {proc.returncode}\n"
                    f"stdout: {proc.stdout}\nstderr: {proc.stderr}")

        notify = NotifySink(tmp / "notify.sock")
        env["NOTIFY_SOCKET"] = str(notify.path)
        with center_log.open("wb") as clog:
            center = subprocess.Popen(
                [str(trigger_center), "--record-manifest", str(manifest),
                 str(config)],
                stdout=clog, stderr=subprocess.STDOUT, env=env,
                preexec_fn=_pdeathsig)
            notify.wait_for("READY=1", READY_DEADLINE_S)
            # READY must not be sent before the recorder is attached: by this
            # point a trigger would be accepted, not rejected as offline.
            notify.wait_for("WATCHDOG=1", WATCHDOG_DEADLINE_S)
            service_log = tmp / "service.log"
            wait_for(
                lambda: service_log.exists() and
                "service log configured" in service_log.read_text(
                    errors="replace"),
                READY_DEADLINE_S, "configured service log record")
            rc = stop_process(center)
            center = None
        if rc != 0:
            raise Failure(f"trigger_center exited {rc} on SIGTERM")
        notify.wait_for("STOPPING=1", STOP_DEADLINE_S)
    except Failure:
        if center_log.exists():
            print(f"--- trigger_center log ---\n"
                  f"{center_log.read_text(errors='replace')}", file=sys.stderr)
        if producer_log.exists():
            print(f"--- demo_producer log ---\n"
                  f"{producer_log.read_text(errors='replace')}", file=sys.stderr)
        raise
    finally:
        if center is not None:
            stop_process(center)
        if producer is not None:
            stop_process(producer)
        if notify is not None:
            notify.close()
        cleanup_shm(tag)
        unlink_session_lease(tmp / "out")
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trigger-center", required=True,
                        help="path to the trigger_center binary")
    parser.add_argument("--demo-producer", required=True,
                        help="path to the demo_producer binary")
    args = parser.parse_args(argv)
    trigger_center = pathlib.Path(args.trigger_center).resolve()
    demo_producer = pathlib.Path(args.demo_producer).resolve()
    for binary in (trigger_center, demo_producer):
        if not os.access(binary, os.X_OK):
            print(f"test_trigger_center_ready: not executable: {binary}",
                  file=sys.stderr)
            return 2
    if not pathlib.Path("/dev/shm").is_dir():
        print("test_trigger_center_ready: SKIP: /dev/shm is unavailable")
        return 0
    try:
        run(trigger_center, demo_producer)
    except Failure as exc:
        print(f"test_trigger_center_ready: FAIL: {exc}", file=sys.stderr)
        return 1
    leftovers = leftover_shm(str(os.getpid()))
    if leftovers:
        print(
            "test_trigger_center_ready: FAIL: SHM cleanup left "
            f"{[str(path) for path in leftovers]}",
            file=sys.stderr)
        return 1
    print("test_trigger_center_ready: READY/WATCHDOG/STOPPING ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
