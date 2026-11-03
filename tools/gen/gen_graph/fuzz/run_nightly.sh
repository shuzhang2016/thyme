#!/usr/bin/env bash
# M8.3 — timed libFuzzer run with crash artifact collection.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
BUILD="${1:-$ROOT/build}"
MAX_TIME="${FUZZ_MAX_SECONDS:-300}"
ARTIFACT_DIR="${2:-$ROOT/fuzz-artifacts}"

FUZZ="$BUILD/tools/gen/gen_graph/fuzz_gen_graph_yaml"
if [[ ! -x "$FUZZ" ]]; then
  echo "missing $FUZZ (build with -DTHYME_ENABLE_FUZZ=ON and Clang)" >&2
  exit 1
fi

mkdir -p "$ARTIFACT_DIR"
mapfile -t CORPUS < <(find "$ROOT/tools/gen/gen_graph/testdata" -name '*.yaml' | sort)
if [[ ${#CORPUS[@]} -eq 0 ]]; then
  echo "no yaml corpus under testdata" >&2
  exit 1
fi

echo "fuzz nightly: max_total_time=${MAX_TIME}s artifacts=$ARTIFACT_DIR corpus=${#CORPUS[@]} files"
set +e
"$FUZZ" \
  -max_total_time="$MAX_TIME" \
  -artifact_prefix="$ARTIFACT_DIR/crash-" \
  "${CORPUS[@]}"
code=$?
set -e

shopt -s nullglob
crashes=( "$ARTIFACT_DIR"/crash-* )
if [[ ${#crashes[@]} -gt 0 ]]; then
  echo "fuzz found ${#crashes[@]} crash artifact(s)" >&2
  ls -la "$ARTIFACT_DIR"
  exit 1
fi

echo "fuzz nightly complete (exit=$code, no new crash artifacts)"
exit 0
