#!/usr/bin/env bash
# Build the OBS-10 eval_chains fixture app (head -> mid -> tail, 200/400 Hz SPSC).
#
# The app skeleton is generated from the committed fixture template, then the
# hand-written pass-through nodes are copied over the generated stubs. The
# result is a normal generated app tree; only `eval_chains` is built.
#
# Usage:
#   tools/ci/eval_chains_build.sh [--gen-graph PATH] [--thyme DIR]
#                                 [--out DIR] [--jobs N]
#
# Prints the eval_chains binary path on success.
set -euo pipefail

GEN_GRAPH=""
THYME=""
OUT=""
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --gen-graph) GEN_GRAPH="$2"; shift 2 ;;
    --thyme) THYME="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,12p' "$0"
      exit 0 ;;
    *)
      echo "eval_chains_build: unknown argument: $1" >&2
      exit 2 ;;
  esac
done

THYME="$(cd "${THYME:-$(pwd)}" && pwd)"
FIXTURE="$THYME/tools/gen/gen_graph/testdata/eval_chains"
GEN_GRAPH="${GEN_GRAPH:-$THYME/build/tools/gen/gen_graph/gen_graph}"
OUT="${OUT:-$THYME/build-eval-chains}"

[ -x "$GEN_GRAPH" ] || {
  echo "eval_chains_build: gen_graph not executable: $GEN_GRAPH" >&2
  echo "  build it first: cmake --build build -j4 --target gen_graph" >&2
  exit 2
}
[ -f "$FIXTURE/graph.yaml" ] || {
  echo "eval_chains_build: fixture missing: $FIXTURE" >&2
  exit 2
}

echo "eval_chains_build: fixture=$FIXTURE out=$OUT"
echo "eval_chains_build: loads (cpu/io/irq) are implemented inside eval_chains; no external stress tool is required"

rm -rf "$OUT"
mkdir -p "$OUT"

"$GEN_GRAPH" --out "$OUT/app" --update all \
  --template-dir "$FIXTURE/template" --in "$FIXTURE/graph.yaml" \
  --no-health-reporter

cp "$FIXTURE"/nodes/* "$OUT/app/src/nodes/"

cmake -S "$OUT/app" -B "$OUT/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DTHYME_SOURCE_DIR="$THYME" >/dev/null
cmake --build "$OUT/build" -j"$JOBS" --target eval_chains >/dev/null

echo "eval_chains_build: $OUT/build/eval_chains"
