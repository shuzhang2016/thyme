#!/usr/bin/env python3
"""TRG-15: the default trigger_center process split still seals a package.

The parent binds the trigger socket and notifies systemd. The child owns the
recorder, reads frames from the producer's shared memory, and is woken with
one eventfd. The datagram that crosses the process boundary is the trigger
command, not the frame.

This test records wake and trigger-to-first-payload for the split path and
for --in-process. It fails if the split does not seal, if the wake was not
observed, or if the split's first-payload time is more than 15 ms above the
in-process sample. It does not treat one machine's wake samples as a board
budget.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import os
import pathlib
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

READY_DEADLINE_S = 15.0
STOP_DEADLINE_S = 10.0
PAYLOAD_SLACK_NS = 15_000_000


class Failure(RuntimeError):
    pass


def wait_for(pred, deadline_s: float, what: str):
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(0.01)
    raise Failure(f"timed out after {deadline_s:.1f}s waiting for {what}")


class NotifySink:
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


def _pdeathsig() -> None:
    try:
        libc = ctypes.CDLL(ctypes.util.find_library("c") or "libc.so.6",
                           use_errno=True)
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


def cpu_ticks(pid: int) -> int:
    text = pathlib.Path(f"/proc/{pid}/stat").read_text()
    rest = text.rsplit(")", 1)[1].split()
    return int(rest[11]) + int(rest[12])


def write_config(path: pathlib.Path, tmp: pathlib.Path, tag: str) -> dict:
    names = {
        "page": f"/thyme_tc_split_{tag}_page",
        "ring": f"/thyme_tc_split_{tag}_ring",
        "chan": f"/thyme_tc_split_{tag}_chan",
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
after_s = 0
priority = 10
mute_s = 0
flush_policy = realtime
quota_class = manual

[quota.manual]
max_triggers_per_hour = 100
""".lstrip(),
        encoding="utf-8",
    )
    return names


def cleanup_shm(tag: str):
    prefix = f"thyme_tc_split_{tag}_"
    shm = pathlib.Path("/dev/shm")
    if not shm.is_dir():
        return
    for path in shm.iterdir():
        if path.name.startswith(prefix):
            try:
                path.unlink()
            except FileNotFoundError:
                pass


def run_mode(trigger_center: pathlib.Path, demo_producer: pathlib.Path,
             triggerctl: pathlib.Path, mode: str) -> dict:
    tag = f"{os.getpid()}_{mode}"
    tmp = pathlib.Path(tempfile.mkdtemp(prefix=f"thyme-tc-split-{tag}-"))
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
    env["THYME_SHM_TRACK"] = "0"
    center_log = tmp / "center.log"
    producer = None
    center = None
    notify = None
    leases_before = set(os.listdir("/dev/shm"))
    try:
        cleanup_shm(tag)
        producer = subprocess.Popen(
            [str(demo_producer), names["page"], names["ring"], names["chan"],
             "100"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
            preexec_fn=_pdeathsig)
        for suffix in ("page", "ring", "chan"):
            shm = pathlib.Path("/dev/shm") / f"thyme_tc_split_{tag}_{suffix}"
            wait_for(lambda p=shm: p.exists(), READY_DEADLINE_S, f"shm {shm}")
        notify = NotifySink(tmp / "notify.sock")
        env["NOTIFY_SOCKET"] = str(notify.path)
        command = [str(trigger_center)]
        if mode == "inproc":
            command.append("--in-process")
        command += ["--record-manifest", str(manifest), str(config)]
        center_log_f = center_log.open("w", encoding="utf-8")
        center = subprocess.Popen(
            command, stdout=center_log_f, stderr=subprocess.STDOUT, env=env,
            preexec_fn=_pdeathsig)
        notify.wait_for("READY=1", READY_DEADLINE_S)
        notify.wait_for("WATCHDOG=1", READY_DEADLINE_S)

        def log_text() -> str:
            return center_log.read_text(errors="replace")

        if mode == "split":
            child_m = wait_for(
                lambda: re.search(r"split child (\d+)", log_text()),
                READY_DEADLINE_S, "split child pid")
            child_pid = int(child_m.group(1))
        else:
            child_pid = 0
        idle_before = cpu_ticks(center.pid)
        if child_pid:
            idle_before += cpu_ticks(child_pid)
        time.sleep(1.0)
        idle_after = cpu_ticks(center.pid)
        if child_pid:
            idle_after += cpu_ticks(child_pid)
        pattern = (r"split wake (\d+) ns first_payload_delta (\d+) ns"
                   if mode == "split"
                   else r"inproc first_payload_delta (\d+) ns")
        wakes: list[int] = []
        deltas: list[int] = []
        for index in range(8):
            # The in-process line is stamped at the first frame, while the
            # center is still CAPTURING. A same-priority trigger in that
            # window is muted. Retry until the previous capture goes idle.
            accepted = False
            deadline = time.monotonic() + READY_DEADLINE_S
            last = None
            while time.monotonic() < deadline:
                last = subprocess.run(
                    [str(triggerctl), "manual", "--socket",
                     str(tmp / "trigger.sock"), "--timeout", "2000"],
                    capture_output=True, text=True, env=env, check=False)
                if last.returncode == 0:
                    accepted = True
                    break
                if "code=muted" not in last.stdout:
                    break
                time.sleep(0.02)
            if not accepted:
                detail = "" if last is None else (
                    f"\nstdout: {last.stdout}\nstderr: {last.stderr}")
                raise Failure(
                    f"{mode} triggerctl did not accept trigger {index + 1}"
                    f"{detail}\nlog: {log_text()}")
            found = wait_for(
                lambda n=index + 1: len(re.findall(pattern, log_text())) >= n,
                READY_DEADLINE_S, f"{mode} payload line {index + 1}")
            if not found:
                raise Failure(f"{mode} missing payload line")
        for match in re.finditer(pattern, log_text()):
            if mode == "split":
                wakes.append(int(match.group(1)))
                deltas.append(int(match.group(2)))
            else:
                deltas.append(int(match.group(1)))
        if len(deltas) != 8:
            raise Failure(f"{mode} expected 8 payload lines, got {deltas}")
        raws = list((tmp / "out").rglob("*.raw"))
        if not raws or max(path.stat().st_size for path in raws) <= 64:
            raise Failure(f"{mode} package has no frame payload\n{log_text()}")
        rc = stop_process(center)
        center = None
        if rc != 0:
            raise Failure(f"{mode} trigger_center exited {rc}\n{log_text()}")
        notify.wait_for("STOPPING=1", STOP_DEADLINE_S)
        return {"wakes": wakes, "deltas": deltas,
                "idle_ticks": idle_after - idle_before}
    except Exception:
        if center_log.exists():
            print(f"--- {mode} log ---\n{center_log.read_text(errors='replace')}",
                  file=sys.stderr)
        raise
    finally:
        if center is not None:
            stop_process(center)
        if producer is not None:
            stop_process(producer)
        if notify is not None:
            notify.close()
        cleanup_shm(tag)
        # ARCH-14: trigger_center and its forked recorder each keep their
        # session-lease page on purpose. Their roots are derived from the
        # temporary out_dir, so a test that deletes the tree would otherwise
        # leave `/dev/shm/tsess_*` behind on every run.
        for name in set(os.listdir("/dev/shm")) - leases_before:
            if name.startswith("tsess_"):
                try:
                    os.unlink(f"/dev/shm/{name}")
                except OSError:
                    pass
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trigger-center", required=True)
    parser.add_argument("--demo-producer", required=True)
    parser.add_argument("--triggerctl", required=True)
    args = parser.parse_args(argv)
    binaries = [pathlib.Path(p).resolve() for p in (
        args.trigger_center, args.demo_producer, args.triggerctl)]
    for binary in binaries:
        if not os.access(binary, os.X_OK):
            print(f"test_trigger_center_split: not executable: {binary}",
                  file=sys.stderr)
            return 2
    if not pathlib.Path("/dev/shm").is_dir():
        print("test_trigger_center_split: SKIP: /dev/shm is unavailable")
        return 0
    def median(values: list[int]) -> int:
        ordered = sorted(values)
        return ordered[len(ordered) // 2]

    try:
        split = run_mode(binaries[0], binaries[1], binaries[2], "split")
        inproc = run_mode(binaries[0], binaries[1], binaries[2], "inproc")
    except Failure as exc:
        print(f"test_trigger_center_split: FAIL: {exc}", file=sys.stderr)
        return 1
    split_delta = median(split["deltas"])
    inproc_delta = median(inproc["deltas"])
    print(
        "test_trigger_center_split: "
        f"wake_ns min={min(split['wakes'])} "
        f"p50={median(split['wakes'])} max={max(split['wakes'])} "
        f"split_delta_ns={split_delta} inproc_delta_ns={inproc_delta} "
        f"split_idle_ticks={split['idle_ticks']} "
        f"inproc_idle_ticks={inproc['idle_ticks']}")
    if min(split["wakes"]) <= 0:
        print("test_trigger_center_split: FAIL: wake was not observed",
              file=sys.stderr)
        return 1
    if split_delta > inproc_delta + PAYLOAD_SLACK_NS:
        print(
            "test_trigger_center_split: FAIL: split first-payload is more "
            f"than {PAYLOAD_SLACK_NS} ns above in-process",
            file=sys.stderr)
        return 1
    print("test_trigger_center_split: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
