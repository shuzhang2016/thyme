#!/usr/bin/env bash
# Generate a CycloneDX SBOM for thyme's direct third-party dependencies and
# check that every one of them has a license entry.
#
# Dependencies are derived from the repository, not from a hard-coded list:
#   1. CMakeLists.txt: project() name/version, find_package(), target_link_-
#      libraries() system libraries, and build/test tool invocations.
#   2. #include <...> of system headers in src/ and include/, classified into
#      libc / libstdc++ / pthreads / Linux UAPI / Winsock. Headers that cannot
#      be classified are reported as an error, so new dependencies cannot be
#      added silently.
#   3. docs/reference/third-party-licenses.md: the license text for each dependency.
#
# Usage:
#   tools/ci/gen_sbom.sh [--output PATH] [--timestamp] [--allow-unknown]
#                        [--inventory PATH]
#
#   --output PATH     write the BOM to PATH instead of stdout
#   --timestamp       add metadata.timestamp (omitted by default so repeated
#                     runs on the same tree are byte-identical)
#   --allow-unknown   emit NOASSERTION for dependencies missing from the
#                     inventory instead of failing. Local debugging only; CI
#                     and release builds must not use it.
#   --inventory PATH  use an alternative inventory document (testing/forked
#                     inventories). Default: docs/reference/third-party-licenses.md.
#
# Exit codes:
#   0  SBOM generated and every dependency has a license entry
#   1  usage error, missing inventory file, or write failure
#   2  dependency/license depth check failed: a dependency found in the build
#      has no inventory entry, or a system header could not be classified
#
# No network access and no new dependencies: bash + python3 (python3 is
# already required by tools/ci/check_doc_links.py).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
  echo "ERROR: gen_sbom.sh requires python3" >&2
  exit 1
fi

exec python3 - "$root" "$@" <<'PYTHON'
import argparse
import json
import os
import re
import subprocess
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve()
ARGV = sys.argv[2:]

CXX_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".inc"}

STD_HEADERS = {
    "algorithm", "any", "array", "atomic", "bitset", "cassert", "cctype",
    "cerrno", "cfenv", "cfloat", "chrono", "cinttypes", "climits", "clocale",
    "cmath", "complex", "condition_variable", "csignal", "cstdarg", "cstddef",
    "cstdint", "cstdio", "cstdlib", "cstring", "ctime", "cuchar", "cwchar",
    "cwctype", "deque", "exception", "filesystem", "forward_list", "fstream",
    "functional", "future", "initializer_list", "iomanip", "ios", "iosfwd",
    "iostream", "istream", "iterator", "limits", "list", "map", "memory",
    "mutex", "new", "numeric", "optional", "ostream", "queue", "random",
    "ratio", "regex", "set", "shared_mutex", "sstream", "stack", "stdexcept",
    "streambuf", "string", "string_view", "system_error", "thread", "tuple",
    "type_traits", "typeindex", "typeinfo", "unordered_map", "unordered_set",
    "utility", "valarray", "variant", "vector",
}

GLIBC_HEADERS = {
    "arpa/inet.h", "dirent.h", "dlfcn.h", "errno.h", "fcntl.h", "glob.h",
    "grp.h", "link.h", "malloc.h", "netdb.h", "netinet/in.h",
    "netinet/tcp.h", "poll.h", "pthread.h", "pwd.h", "sched.h", "semaphore.h",
    "signal.h", "stdio.h", "stdlib.h", "string.h", "sys/auxv.h", "sys/eventfd.h",
    "sys/file.h", "sys/ioctl.h", "sys/mman.h", "sys/prctl.h", "sys/ptrace.h",
    "sys/resource.h", "sys/select.h", "sys/socket.h", "sys/stat.h", "sys/statvfs.h",
    "sys/syscall.h", "sys/time.h", "sys/timerfd.h", "sys/types.h", "sys/ucred.h",
    "sys/uio.h", "sys/un.h", "sys/utsname.h", "sys/wait.h", "time.h", "unistd.h",
}
PTHREAD_HEADERS = {"pthread.h"}
LINUX_UAPI_HEADERS = {"linux/futex.h", "asm/hwcap.h"}
LINUX_UAPI_PREFIXES = ("linux/", "asm/")
WIN_HEADERS = {"io.h", "winsock2.h", "ws2tcpip.h", "windows.h", "winsock.h"}
COMPILER_HEADERS_PREFIXES = (
    "immintrin.h", "emmintrin.h", "xmmintrin.h", "cpuid.h",
    "arm_acle.h", "arm_neon.h", "arm_fp16.h", "avx2intrin.h",
)

# key -> component metadata and aliases used to find the inventory row.
DEPS = {
    "cmake": {
        "name": "cmake",
        "aliases": ["cmake"],
        "scope": "required",
        "version_probe": ["cmake", "--version"],
        "version_filter": lambda out: out.splitlines()[0].split()[-1],
    },
    "glibc": {
        "name": "glibc",
        "aliases": ["libc", "glibc"],
        "scope": "required",
        "version_probe": ["getconf", "GNU_LIBC_VERSION"],
        "version_filter": lambda out: out.split()[-1],
    },
    "libstdc++": {
        "name": "libstdc++",
        "aliases": ["libstdc++", "libc++"],
        "scope": "required",
        "version_probe": None,
        "version_filter": None,
    },
    "linux-futex-rt": {
        "name": "linux-futex-rt",
        "aliases": ["futex", "linux futex/rt", "librt"],
        "scope": "required",
        "version_probe": None,
        "version_filter": None,
    },
    "pthreads": {
        "name": "pthreads",
        "aliases": ["pthreads", "pthread"],
        "scope": "required",
        "version_probe": ["getconf", "GNU_LIBPTHREAD_VERSION"],
        "version_filter": lambda out: out.split()[-1],
    },
    "python3": {
        "name": "python3",
        "aliases": ["python3", "python 3", "cpython"],
        "scope": "required",
        "version_probe": ["python3", "--version"],
        "version_filter": lambda out: out.split()[-1],
    },
    "winsock2": {
        "name": "winsock2",
        "aliases": ["winsock", "ws2_32", "ws2-32"],
        "scope": "optional",
        "version_probe": None,
        "version_filter": None,
    },
}

SPDX_IDS = {
    "0BSD", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "BSL-1.0",
    "ISC", "LGPL-2.1-only", "LGPL-3.0-only", "MIT", "MPL-2.0", "PSF-2.0",
    "Unlicense", "Zlib",
}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.print_usage(sys.stderr)
        print(f"{self.prog}: error: {message}", file=sys.stderr)
        sys.exit(1)


def parse_args():
    parser = Parser(
        prog="gen_sbom.sh",
        description="Generate a CycloneDX SBOM for thyme direct dependencies.",
    )
    parser.add_argument("--output", metavar="PATH", help="write BOM to PATH")
    parser.add_argument(
        "--timestamp",
        action="store_true",
        help="include metadata.timestamp (omitted by default)",
    )
    parser.add_argument(
        "--allow-unknown",
        action="store_true",
        help="emit NOASSERTION instead of failing on missing inventory rows",
    )
    parser.add_argument(
        "--inventory",
        metavar="PATH",
        default=str(ROOT / "docs" / "reference" / "third-party-licenses.md"),
        help="license inventory document (default: docs/reference/third-party-licenses.md)",
    )
    return parser.parse_args(ARGV)


def project_metadata(cmake_text):
    match = re.search(
        r"project\s*\(\s*([A-Za-z0-9_.+-]+)\s+VERSION\s+([0-9]+(?:\.[0-9]+)*)",
        cmake_text,
    )
    if not match:
        return "thyme", "unknown"
    return match.group(1), match.group(2)


def parse_inventory(path):
    """Return [(label, license_text)] from the Markdown dependency table."""
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if len(cells) < 3:
            continue
        label = cells[0]
        if not label or set(label) <= set("-: "):
            continue
        if label.lower() in ("dependency", "dependencies", "name", "component"):
            continue
        rows.append((label, cells[2].strip().strip("`")))
    return rows


def alias_matches(alias, label):
    pattern = r"(?<![a-z0-9+])" + re.escape(alias) + r"(?![a-z0-9+])"
    return re.search(pattern, label) is not None


def find_inventory_row(rows, aliases):
    for label, license_text in rows:
        normalized = label.lower().replace("（", "(").replace("）", ")")
        for alias in aliases:
            if alias_matches(alias, normalized):
                return label, license_text
    return None


def probe_version(dep):
    command = dep.get("version_probe")
    version_filter = dep.get("version_filter")
    if not command or not version_filter:
        return "unknown"
    env = dict(os.environ)
    env["LC_ALL"] = "C"
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=10, env=env
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    if result.returncode != 0:
        return "unknown"
    try:
        version = version_filter(result.stdout.strip()).strip()
    except (IndexError, AttributeError):
        return "unknown"
    return version or "unknown"


def evidence_value(paths):
    items = sorted(paths)
    shown = "; ".join(items[:5])
    if len(items) > 5:
        shown += f"; +{len(items) - 5} more"
    return shown


def license_entry(text):
    if text in SPDX_IDS:
        return {"license": {"id": text}}
    return {"license": {"name": text}}


def main():
    args = parse_args()

    inventory_path = Path(args.inventory).resolve()
    if not inventory_path.is_file():
        print(f"ERROR: license inventory not found: {inventory_path}", file=sys.stderr)
        return 1

    cmake_path = ROOT / "CMakeLists.txt"
    if not cmake_path.is_file():
        print(f"ERROR: {cmake_path} not found", file=sys.stderr)
        return 1
    cmake_text = cmake_path.read_text(encoding="utf-8", errors="replace")
    project_name, project_version = project_metadata(cmake_text)

    # key -> set of evidence strings
    found = {}

    def add(key, evidence):
        found.setdefault(key, set()).add(evidence)

    # --- CMake-derived dependencies -------------------------------------
    if "cmake_minimum_required" in cmake_text:
        add("cmake", "CMakeLists.txt (cmake_minimum_required)")
    if re.search(r"project\s*\([^)]*LANGUAGES[^)]*\bCXX\b", cmake_text, re.S):
        add("libstdc++", "CMakeLists.txt (LANGUAGES CXX)")
        add("glibc", "CMakeLists.txt (LANGUAGES CXX)")
    if re.search(r"find_package\s*\(\s*Threads\b", cmake_text):
        add("pthreads", "CMakeLists.txt (find_package(Threads))")
    cmake_dir = ROOT / "cmake"
    if cmake_dir.is_dir():
        for cmake_file in sorted(cmake_dir.rglob("*.cmake*")):
            if not cmake_file.is_file():
                continue
            text = cmake_file.read_text(encoding="utf-8", errors="replace")
            if re.search(r"find_dependency\s*\(\s*Threads\b", text):
                add(
                    "pthreads",
                    f"{cmake_file.relative_to(ROOT).as_posix()} "
                    "(find_dependency(Threads))",
                )
    if re.search(r"target_link_libraries\s*\([^)]*\brt\b", cmake_text, re.S):
        add("linux-futex-rt", "CMakeLists.txt (target_link_libraries ... rt)")
    if re.search(r"\bpython3\b", cmake_text):
        add("python3", "CMakeLists.txt (add_test uses python3)")

    # --- header-derived dependencies ------------------------------------
    include_re = re.compile(r"^\s*#\s*include\s*<([^>]+)>")
    unmapped = []
    for base in ("src", "include"):
        base_path = ROOT / base
        if not base_path.is_dir():
            continue
        for source in sorted(base_path.rglob("*")):
            if not source.is_file() or source.suffix not in CXX_SUFFIXES:
                continue
            rel = source.relative_to(ROOT).as_posix()
            for line in source.read_text(encoding="utf-8", errors="replace").splitlines():
                match = include_re.match(line)
                if not match:
                    continue
                header = match.group(1).strip()
                if header in STD_HEADERS:
                    add("libstdc++", rel)
                elif header in PTHREAD_HEADERS:
                    add("pthreads", rel)
                elif header in LINUX_UAPI_HEADERS or header.startswith(LINUX_UAPI_PREFIXES):
                    add("linux-futex-rt", rel)
                elif header in WIN_HEADERS:
                    add("winsock2", rel)
                elif header in GLIBC_HEADERS:
                    add("glibc", rel)
                elif header.startswith(COMPILER_HEADERS_PREFIXES):
                    # Compiler intrinsics ship with the toolchain itself.
                    continue
                else:
                    unmapped.append(f"{rel}: unmapped system header <{header}>")

    rows = parse_inventory(inventory_path)
    components = []
    problems = []

    for key in sorted(found):
        dep = DEPS[key]
        evidence = evidence_value(found[key])
        row = find_inventory_row(rows, dep["aliases"])
        if row is None:
            problems.append(
                f"{dep['name']}: no entry in {inventory_path.name} "
                f"(evidence: {evidence})"
            )
            licenses = [{"license": {"name": "NOASSERTION"}}]
        else:
            licenses = [license_entry(row[1])]

        version = probe_version(dep)
        component = {
            "type": "library",
            "bom-ref": f"thyme:dep:{key}",
            "name": dep["name"],
            "version": version,
            "scope": dep["scope"],
            "licenses": licenses,
            "properties": [{"name": "thyme:evidence", "value": evidence}],
        }
        components.append(component)

    if unmapped:
        problems.extend(unmapped)

    if problems:
        if args.allow_unknown:
            print(
                "SBOM dependency depth check failed (continuing because "
                "--allow-unknown):",
                file=sys.stderr,
            )
        else:
            print("SBOM dependency depth check failed:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "Add the dependency to docs/reference/third-party-licenses.md (or use "
            "--allow-unknown for a local-only SBOM).",
            file=sys.stderr,
        )
        if not args.allow_unknown:
            return 2

    components.sort(key=lambda component: component["name"])
    canonical = json.dumps(components, sort_keys=True, separators=(",", ":"))
    serial = uuid.uuid5(uuid.NAMESPACE_URL, "thyme-sbom:" + canonical)

    metadata = {
        "tools": {
            "components": [
                {
                    "type": "application",
                    "name": "gen_sbom.sh",
                    "version": "1.0",
                }
            ]
        },
        "component": {
            "type": "library",
            "bom-ref": f"thyme:component:{project_name}@{project_version}",
            "name": project_name,
            "version": project_version,
            "properties": [
                {
                    "name": "thyme:root-license",
                    "value": "MIT",
                }
            ],
        },
    }
    if args.timestamp:
        metadata["timestamp"] = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime()
        )

    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": metadata,
        "components": components,
    }

    output = json.dumps(bom, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        output_path = Path(args.output)
        try:
            output_path.write_text(output, encoding="utf-8")
        except OSError as error:
            print(f"ERROR: cannot write {output_path}: {error}", file=sys.stderr)
            return 1
        print(
            f"SBOM written to {output_path} ({len(components)} components)",
            file=sys.stderr,
        )
    else:
        sys.stdout.write(output)
    return 0


sys.exit(main())
PYTHON
