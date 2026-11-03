#!/usr/bin/env python3
"""Run clang-tidy over every production translation unit exactly once."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parents[2]
DIAGNOSTIC = re.compile(
    r"^(?P<path>.*?):(?P<line>\d+):(?P<column>\d+):\s+"
    r"(?P<severity>warning|error):\s+(?P<message>.*?)\s+"
    r"\[(?P<check>[^]]+)\]$"
)


@dataclass(frozen=True)
class Result:
    source: pathlib.Path
    returncode: int
    output: str


def positive_jobs(value: str) -> int:
    jobs = int(value)
    if jobs < 1:
        raise argparse.ArgumentTypeError("jobs must be at least 1")
    return jobs


def production_sources(build_dir: pathlib.Path) -> list[pathlib.Path]:
    database = build_dir / "compile_commands.json"
    with database.open(encoding="utf-8") as stream:
        commands = json.load(stream)
    sources: dict[pathlib.Path, None] = {}
    for entry in commands:
        directory = pathlib.Path(entry["directory"])
        source = pathlib.Path(entry["file"])
        if not source.is_absolute():
            source = directory / source
        source = source.resolve()
        try:
            source.relative_to(ROOT / "src")
        except ValueError:
            continue
        sources[source] = None
    if not sources:
        raise ValueError(f"no src/ translation units in {database}")
    return sorted(sources)


def run_one(clang_tidy: str, build_dir: pathlib.Path,
            source: pathlib.Path) -> Result:
    command = [clang_tidy, "-p", str(build_dir), str(source)]
    try:
        process = subprocess.run(
            command,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            check=False,
        )
        return Result(source, process.returncode, process.stdout)
    except OSError as error:
        return Result(source, 127, f"{error}\n")


def display_path(path: pathlib.Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path,
                        default=ROOT / "build-tidy")
    parser.add_argument("--clang-tidy", default="clang-tidy-14")
    parser.add_argument("-j", "--jobs", type=positive_jobs,
                        default=os.cpu_count() or 1)
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()

    try:
        sources = production_sources(build_dir)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"clang-tidy runner: {error}", file=sys.stderr)
        return 1

    results: list[Result] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_one, args.clang_tidy, build_dir, source)
                   for source in sources]
        for future in concurrent.futures.as_completed(futures):
            results.append(future.result())

    diagnostics = {}
    failures = []
    for result in results:
        if result.returncode != 0:
            failures.append(result)
        for line in result.output.splitlines():
            match = DIAGNOSTIC.match(line)
            if match is None:
                continue
            path = pathlib.Path(match.group("path")).resolve()
            try:
                path.relative_to(ROOT / "src")
            except ValueError:
                try:
                    path.relative_to(ROOT / "include")
                except ValueError:
                    continue
            line_number = int(match.group("line"))
            key = (path, line_number, match.group("check"))
            if key not in diagnostics:
                diagnostics[key] = (match, result.source)

    for (path, line_number, check), (match, source) in sorted(
            diagnostics.items(), key=lambda item: (str(item[0][0]), item[0][1],
                                                  item[0][2])):
        print(f"{display_path(path)}:{line_number}:{match.group('column')}: "
              f"{match.group('severity')}: {match.group('message')} [{check}] "
              f"(TU: {display_path(source)})")

    for result in sorted(failures, key=lambda item: str(item.source)):
        print(f"clang-tidy failed ({result.returncode}): "
              f"{display_path(result.source)}", file=sys.stderr)
        if result.output:
            print(result.output, end="" if result.output.endswith("\n") else "\n",
                  file=sys.stderr)

    print(f"clang-tidy: {len(sources)} translation units, "
          f"{len(diagnostics)} unique diagnostics, {len(failures)} failed tools")
    return 1 if diagnostics or failures else 0


if __name__ == "__main__":
    sys.exit(main())
