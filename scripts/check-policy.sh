#!/usr/bin/env bash
# Repository policies, checked in CI (ci.yml, "Policy and format").
set -euo pipefail
cd "$(dirname "$0")/.."
fail=0
sources() { find include src tests fuzz bench \( -name '*.h' -o -name '*.cc' \) -print; }
# 1. The library must not replace the global allocator: that would change how its host allocates memory.
if sources | xargs grep -nE '^[^/]*(void[[:space:]]*\*?|void)[[:space:]]+operator[[:space:]]+(new|delete)'; then
  echo "::error::global operator new/delete replacement found"; fail=1
fi
# 2. Every source file states its licence.
while IFS= read -r f; do
  grep -q 'SPDX-License-Identifier' "$f" || { echo "::error file=$f::missing SPDX-License-Identifier"; fail=1; }
done < <(sources)
[ "$fail" -eq 0 ] && echo "policy checks passed"
exit "$fail"
