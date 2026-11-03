#!/usr/bin/env bash
# CI-04 / TST-01 — nightly coverage: build an instrumented tree, run the FULL
# ctest suite, export per-line data, generate the report, and enforce the
# per-library gate in tools/ci/coverage_thresholds.json.
#
# Clang trees export llvm-cov JSON; GCC trees export lcov .info. The exporter
# picks the format from what the build produced, never both.
#
# Usage: run_nightly.sh [build-dir] [report-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="${1:-$ROOT/build-cov}"
OUT="${2:-$ROOT/coverage-report}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
  if command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
  else
    CXX=c++
  fi
fi

mkdir -p "$OUT"
# CI-06 reads these from the artifact; ctest resolves a relative JUnit path
# against the build directory, so pin an absolute one.
OUT="$(cd "$OUT" && pwd)"

echo "coverage nightly: build=$BUILD out=$OUT compiler=$CXX jobs=$JOBS"
cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTHYME_ENABLE_COVERAGE=ON
cmake --build "$BUILD" -j"$JOBS"

# One profile per process so parallel ctest workers do not clobber each other.
export LLVM_PROFILE_FILE="$BUILD/cov-%p.profraw"
rm -f "$BUILD"/cov-*.profraw "$BUILD"/cov.profdata
find "$BUILD" -name '*.gcda' -delete

# CI-06 evidence: test labels plus a JUnit result file, published alongside
# the coverage report so the acceptance job can build its table from
# artifacts instead of re-running the suite.
ctest --test-dir "$BUILD" --show-only=json-v1 > "$OUT/ctest-tests.json" || true

ctest_rc=0
ctest --test-dir "$BUILD" --output-on-failure -j"$JOBS" \
  --output-junit "$OUT/ctest-junit.xml" || ctest_rc=$?

export_rc=0
EXPORT="$(bash "$ROOT/tools/ops/coverage/export_coverage.sh" \
  --build-dir "$BUILD" --out-dir "$OUT" --report | tail -1)" || export_rc=$?
if [[ "$export_rc" -ne 0 || -z "$EXPORT" ]]; then
  echo "coverage export failed" >&2
  exit "$export_rc"
fi

gate_rc=0
python3 "$ROOT/tools/ci/check_coverage.py" \
  --build-dir "$BUILD" --export "$EXPORT" | tee "$OUT/gate.txt" || gate_rc=$?

if [[ "$ctest_rc" -ne 0 ]]; then
  echo "full ctest suite failed (exit $ctest_rc)" >&2
  exit "$ctest_rc"
fi
if [[ "$gate_rc" -ne 0 ]]; then
  echo "coverage gate failed (exit $gate_rc)" >&2
  exit "$gate_rc"
fi
echo "coverage report and gate OK: $OUT"
