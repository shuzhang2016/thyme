#!/usr/bin/env python3
"""Read-only preflight for repeatable host-side test runs."""
from __future__ import annotations

import argparse
import os
import pathlib
import sys


def orphan_processes(proc_root: pathlib.Path) -> list[tuple[int, str]]:
    orphans: list[tuple[int, str]] = []
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text(encoding="utf-8")
            left = stat.index("(")
            right = stat.rindex(")")
            comm = stat[left + 1:right]
            ppid = int(stat[right + 1:].split()[1])
        except (OSError, ValueError, IndexError):
            continue
        if ppid == 1 and (comm in {"yes", "demo_producer"} or
                          comm.startswith("test_")):
            orphans.append((int(entry.name), comm))
    return sorted(orphans)


def shm_stats(shm_dir: pathlib.Path) -> tuple[int, int]:
    stats = os.statvfs(shm_dir)
    return len(list(shm_dir.iterdir())), stats.f_bavail * stats.f_frsize


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proc-root", default="/proc")
    parser.add_argument("--shm-dir", default="/dev/shm")
    args = parser.parse_args(argv)

    load = os.getloadavg()
    print(f"loadavg: {load[0]:.2f} {load[1]:.2f} {load[2]:.2f}")
    orphans = orphan_processes(pathlib.Path(args.proc_root))
    for pid, comm in orphans:
        print(f"orphan: pid={pid} comm={comm}", file=sys.stderr)
    shm_dir = pathlib.Path(args.shm_dir)
    if shm_dir.is_dir():
        count, free_bytes = shm_stats(shm_dir)
        print(f"shm: objects={count} free={free_bytes / (1024 * 1024):.1f} MiB")
    else:
        print(f"shm: {shm_dir} unavailable")
    if orphans:
        print(f"test environment: FAIL ({len(orphans)} orphan workload processes)",
              file=sys.stderr)
        return 1
    print("test environment: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
