#!/usr/bin/env python3
"""Regression guard: no test may leak POSIX shm objects.

Runs every non-excluded ctest case in isolation, comparing /dev/shm before and
after. A leak means the test created a segment and did not remove it — over
many runs the tmpfs fills and any test that faults a freshly written page dies
with SIGBUS (the failure mode this guard exists to prevent).

Tests must be run one at a time (the ctest entry is RUN_SERIAL) so the snapshot
is not polluted by concurrent cases. Excluded by default: nested-build/fuzz/
bench/storm/fault cases, which are slow and/or deliberately outlive their C++
process.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys

DEFAULT_EXCLUDE = (
    r"gen_graph|fuzz|bench_|storm|fault_matrix|empty_app|trigger_storm_soak|shm_leak_audit"
)


def ctest_names(build_dir: pathlib.Path) -> list[str]:
    out = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "-N"],
        capture_output=True, text=True, check=True).stdout
    names = []
    for line in out.splitlines():
        m = re.match(r"\s*Test\s+#\d+:\s+(\S+)", line)
        if m:
            names.append(m.group(1))
    return names


def snapshot() -> set[str]:
    return set(os.listdir("/dev/shm"))


def _mapped_by_any_process(path: str) -> bool:
    """True when some process has this /dev/shm object mapped.

    Session-lease pages are named by a hash of the out_dir (`tsess_*`), so they
    carry no pid and cannot be attributed by name. A page nobody maps is stale:
    the test that created it has exited.
    """
    needle = path.encode()
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/maps", "rb") as fh:
                if needle in fh.read():
                    return True
        except OSError:
            continue
    return False


def sweep_dead_owners() -> None:
    """Remove stale test segments.

    Pid-suffixed names are removed when their pid is gone; `tsess_*` lease pages
    are removed when no process maps them and the file belongs to us. Both keep
    the audit's before/after windows attributable.
    """
    uid = os.getuid()
    for name in os.listdir("/dev/shm"):
        full = str(pathlib.Path("/dev/shm") / name)
        m = re.search(r"_(\d+)(?:\.lease)?$", name)
        if m:
            pid = int(m.group(1))
            if pid > 1 and not os.path.exists(f"/proc/{pid}"):
                try:
                    os.unlink(full)
                except OSError:
                    pass
            continue
        if not name.startswith("tsess_"):
            continue
        try:
            if os.stat(full).st_uid != uid:
                continue
        except OSError:
            continue
        if not _mapped_by_any_process(full):
            try:
                os.unlink(full)
            except OSError:
                pass


def run_one(build_dir: pathlib.Path, name: str, timeout: int) -> tuple[int, str]:
    proc = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "-R", f"^{re.escape(name)}$",
         "--output-on-failure"],
        capture_output=True, text=True, timeout=timeout)
    return proc.returncode, proc.stdout + proc.stderr


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--exclude", default=DEFAULT_EXCLUDE)
    ap.add_argument("--timeout", type=int, default=180,
                    help="per-test timeout in seconds")
    ap.add_argument("--list", action="store_true",
                    help="only print the cases that would run")
    args = ap.parse_args(argv)

    build = pathlib.Path(args.build_dir).resolve()
    if not build.is_dir():
        print(f"check_shm_leaks: no build dir {build}", file=sys.stderr)
        return 2
    if not pathlib.Path("/dev/shm").is_dir():
        print("check_shm_leaks: SKIP: /dev/shm is unavailable")
        return 0
    excl = re.compile(args.exclude)
    names = [n for n in ctest_names(build) if not excl.search(n)]
    if args.list:
        print("\n".join(names))
        return 0

    preflight = pathlib.Path(__file__).with_name("check_test_environment.py")
    if subprocess.run([sys.executable, str(preflight)], check=False).returncode != 0:
        print("check_shm_leaks: environment preflight failed", file=sys.stderr)
        return 1

    sweep_dead_owners()
    failures: list[str] = []
    leaks: list[tuple[str, list[str]]] = []
    for name in names:
        sweep_dead_owners()
        before = snapshot()
        rc, out = run_one(build, name, args.timeout)
        after = snapshot()
        grew = sorted(after - before)
        if rc != 0:
            failures.append(name)
            print(f"FAIL {name}\n{out[-2000:]}", file=sys.stderr)
        if grew:
            leaks.append((name, grew))
            print(f"LEAK {name}: {grew[:8]}"
                  f"{' …' if len(grew) > 8 else ''}", file=sys.stderr)
            # Remove what the test leaked so later cases start clean.
            for item in grew:
                try:
                    os.unlink(str(pathlib.Path('/dev/shm') / item))
                except OSError:
                    pass
    print(f"check_shm_leaks: {len(names)} cases, "
          f"{len(failures)} failing, {len(leaks)} leaking")
    return 1 if (failures or leaks) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
