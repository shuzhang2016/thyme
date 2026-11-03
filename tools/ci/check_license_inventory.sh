#!/usr/bin/env bash
# Keep the license inventory complete and consistent with the shipped files.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

rc=0

if [[ ! -f NOTICE ]]; then
  echo "ERROR: NOTICE is missing" >&2
  rc=1
fi
if [[ ! -f docs/reference/third-party-licenses.md ]]; then
  echo "ERROR: docs/reference/third-party-licenses.md is missing" >&2
  rc=1
fi

if find . -maxdepth 4 -type d \
    \( -name third_party -o -name third-party -o -name vendor -o -name external \) \
    -not -path './.git/*' -not -path './build*/*' | grep -q .; then
  echo "ERROR: vendored dependency directory found; add it to the inventory" >&2
  rc=1
fi

if [[ ! -f LICENSE ]]; then
  echo "ERROR: root LICENSE is missing" >&2
  rc=1
fi

exit "$rc"
