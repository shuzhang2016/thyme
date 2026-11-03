#!/usr/bin/env python3
"""Run one CTest command with process-group and SHM cleanup."""

from __future__ import annotations

import ctypes
import os
import pathlib
import re
import signal
import subprocess
import sys
import time


def shm_dir() -> pathlib.Path:
    return pathlib.Path(os.environ.get("THYME_TEST_SHM_DIR", "/dev/shm"))


def unlink_shm(path: pathlib.Path) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    except OSError as error:
        print(f"test launcher: cannot unlink {path}: {error}", file=sys.stderr)


def pid_running(pid: int) -> bool:
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
        right = stat.rindex(")")
        return stat[right + 1:].split()[0] != "Z"
    except (OSError, ValueError, IndexError):
        return False


def cleanup_names(prefix: str, owner_pids: set[int]) -> None:
    root = shm_dir()
    if not root.is_dir():
        return
    try:
        entries = list(root.iterdir())
    except OSError:
        return
    for entry in entries:
        matched = bool(prefix) and entry.name.startswith(prefix)
        if not matched:
            candidates = {int(match.group(1))
                          for match in re.finditer(r"_(\d+)", entry.name)}
            candidates.update(owner_pids)
            for pid in candidates:
                if pid <= 1 or pid_running(pid):
                    continue
                marker = f"_{pid}"
                at = entry.name.find(marker)
                if at < 0:
                    continue
                end = at + len(marker)
                if end == len(entry.name) or entry.name[end] in "._-":
                    matched = True
                    break
        if matched:
            unlink_shm(entry)


def descendants(root_pid: int) -> set[int]:
    children: dict[int, list[int]] = {}
    try:
        proc_entries = list(pathlib.Path("/proc").iterdir())
    except OSError:
        return {root_pid}
    for entry in proc_entries:
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text(encoding="utf-8")
            right = stat.rindex(")")
            ppid = int(stat[right + 1:].split()[1])
        except (OSError, ValueError, IndexError):
            continue
        children.setdefault(ppid, []).append(int(entry.name))
    result = {root_pid}
    pending = [root_pid]
    while pending:
        parent = pending.pop()
        for child in children.get(parent, []):
            if child not in result:
                result.add(child)
                pending.append(child)
    return result


def terminate_group(child: subprocess.Popen) -> None:
    def group_exists() -> bool:
        try:
            os.killpg(child.pid, 0)
            return True
        except ProcessLookupError:
            return False

    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + 1.0
    while group_exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    if group_exists():
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def enable_subreaper() -> None:
    if sys.platform.startswith("linux"):
        ctypes.CDLL(None).prctl(24, 1, 0, 0, 0)


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: test_launcher.py <command> [args...]", file=sys.stderr)
        return 2
    base = os.environ.get("THYME_SHM_TEST_PREFIX", "thyme_test_")
    prefix = f"{base}{os.getpid()}_"
    cleanup_names(prefix, set())
    env = dict(os.environ)
    env["THYME_SHM_TEST_PREFIX"] = prefix
    env["THYME_SHM_TRACK"] = "1"
    enable_subreaper()
    child = subprocess.Popen(argv, env=env, start_new_session=True)
    owner_pids = {child.pid}
    signalled = 0

    def request_stop(signum, _frame) -> None:
        nonlocal signalled
        if signalled == 0:
            signalled = signum
            terminate_group(child)

    previous_term = signal.signal(signal.SIGTERM, request_stop)
    previous_int = signal.signal(signal.SIGINT, request_stop)
    try:
        while child.poll() is None:
            owner_pids.update(descendants(child.pid))
            if signalled != 0:
                terminate_group(child)
            time.sleep(0.02)
        owner_pids.update(descendants(child.pid))
        if signalled != 0:
            terminate_group(child)
        else:
            # A successful test can still accidentally leave a daemon behind.
            terminate_group(child)
    finally:
        owner_pids.update(descendants(child.pid))
        if child.poll() is None:
            terminate_group(child)
        child.wait()
        cleanup_names(prefix, owner_pids)
        signal.signal(signal.SIGTERM, previous_term)
        signal.signal(signal.SIGINT, previous_int)
    return 128 + signalled if signalled != 0 else child.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
