#!/usr/bin/env bash
# Export per-line coverage from a THYME_ENABLE_COVERAGE build.
#
# The compiler chosen at configure time decides the native format: clang emits
# LLVM profile data (merged with llvm-profdata, exported with llvm-cov), gcc
# emits .gcda (captured with lcov). Exactly one format is produced, depending
# on the tree that was built. tools/ci/check_coverage.py understands both.
#
# Usage: export_coverage.sh [--build-dir DIR] [--out-dir DIR] [--report]
# Prints the path of the generated export on stdout (last line).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="$ROOT/build-cov"
OUT=""
REPORT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD="$2"; shift 2 ;;
    --out-dir) OUT="$2"; shift 2 ;;
    --report) REPORT=1; shift ;;
    -h|--help)
      sed -n '2,12p' "$0"
      exit 0
      ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

BUILD="$(cd "$BUILD" 2>/dev/null && pwd)" || {
  echo "export_coverage: build dir not found" >&2
  exit 2
}
OUT="${OUT:-$BUILD}"
mkdir -p "$OUT"

# Find a versioned tool next to the compiler (Ubuntu ships llvm-cov-14 while
# the plain llvm-cov name may not exist).
find_llvm_tool() {
  local candidate
  for candidate in "$@"; do
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

LLVM_SUFFIX=""
if command -v clang++ >/dev/null 2>&1; then
  LLVM_SUFFIX="$(clang++ --version 2>/dev/null | head -1 |
    sed -n 's/.*version \([0-9][0-9]*\).*/\1/p')"
elif ls /usr/bin/clang++-* >/dev/null 2>&1; then
  LLVM_SUFFIX="$(ls /usr/bin/clang++-* | sort -V | tail -1 |
    sed 's/.*clang++-//')"
fi
if [[ -n "$LLVM_SUFFIX" ]]; then
  # Prefer the tools matching the configured compiler's major version.
  LLVM_PROFDATA="$(find_llvm_tool "llvm-profdata-$LLVM_SUFFIX" \
    llvm-profdata)" || true
  LLVM_COV="$(find_llvm_tool "llvm-cov-$LLVM_SUFFIX" llvm-cov)" || true
else
  LLVM_PROFDATA="$(find_llvm_tool llvm-profdata)" || true
  LLVM_COV="$(find_llvm_tool llvm-cov)" || true
fi

# ---------------------------------------------------------------- LLVM path
if [[ -n "${LLVM_PROFDATA:-}" && -n "${LLVM_COV:-}" ]]; then
  shopt -s nullglob
  profraw=( "$BUILD"/cov-*.profraw )
  shopt -u nullglob
  if [[ ${#profraw[@]} -gt 0 ]]; then
    PROFDATA="$OUT/cov.profdata"
    echo "export_coverage: llvm-profdata merge (${#profraw[@]} profiles)" >&2
    "$LLVM_PROFDATA" merge -sparse "${profraw[@]}" -o "$PROFDATA"

    # Every test binary carries the coverage map of the library objects it
    # linked. Union the executables that were actually run.
    mapfile -t OBJECTS < <(
      find "$BUILD" -maxdepth 1 -type f -perm -u+x -name 'test_*' |
        sort
    )
    if [[ ${#OBJECTS[@]} -eq 0 ]]; then
      echo "export_coverage: no test executables under $BUILD" >&2
      exit 1
    fi

    EXPORT="$OUT/coverage-export.json"
    echo "export_coverage: llvm-cov export (${#OBJECTS[@]} binaries)" >&2
    "$LLVM_COV" export "${OBJECTS[@]}" \
      -instr-profile="$PROFDATA" \
      -format=text \
      -ignore-filename-regex='(/tests/|/build[^/]*/CMakeFiles/)' \
      > "$EXPORT"
    if [[ "$REPORT" -eq 1 ]]; then
      "$LLVM_COV" report "${OBJECTS[@]}" -instr-profile="$PROFDATA" \
        -ignore-filename-regex='(/tests/|/build[^/]*/CMakeFiles/)' \
        > "$OUT/summary.txt"
      "$LLVM_COV" show "${OBJECTS[@]}" -instr-profile="$PROFDATA" \
        -format=html -output-dir="$OUT/html" \
        -ignore-filename-regex='(/tests/|/build[^/]*/CMakeFiles/)'
    fi
    echo "$EXPORT"
    exit 0
  fi
fi

# ---------------------------------------------------------------- gcov path
if find "$BUILD" -name '*.gcda' -print -quit | grep -q .; then
  if ! command -v lcov >/dev/null 2>&1; then
    echo "export_coverage: gcc build has .gcda data but lcov is not installed" >&2
    exit 2
  fi
  RAW="$OUT/coverage-raw.info"
  EXPORT="$OUT/coverage.info"
  echo "export_coverage: lcov capture" >&2
  lcov --capture --directory "$BUILD" --output-file "$RAW" \
    --ignore-errors gcov,source \
    >/dev/null
  lcov --extract "$RAW" "$ROOT/src/*" --output-file "$EXPORT" \
    --ignore-errors source \
    >/dev/null
  rm -f "$RAW"
  if [[ "$REPORT" -eq 1 ]]; then
    lcov --summary "$EXPORT" > "$OUT/summary.txt" 2>&1 || true
    if command -v genhtml >/dev/null 2>&1; then
      genhtml "$EXPORT" --output-directory "$OUT/html" >/dev/null 2>&1 || true
    fi
  fi
  echo "$EXPORT"
  exit 0
fi

echo "export_coverage: no coverage data under $BUILD" >&2
echo "export_coverage: configure with -DTHYME_ENABLE_COVERAGE=ON, build, then run ctest" >&2
exit 1
