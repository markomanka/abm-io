#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Runs abm-gen on BioDynaMo's own agent and behaviour classes (issue #10) and compares
# the result with tests/generator/biodynamo/expected.txt, checked by hand.
#
#   scripts/abm-gen-biodynamo.sh BDM_DIR [OUT]
#
# BDM_DIR is a checkout of the fork at the commit in tests/generator/biodynamo/FORK_COMMIT
# (src/ and third_party/cpptoml/ are enough: headers only, no build). ROOT's and Boost's
# headers and a Clang of libclang's major version come from the active conda environment
# ($CONDA_PREFIX). With OUT, the output is also written there, to update expected.txt.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
bdm="$(cd "$1" && pwd)"
dir="$here/tests/generator/biodynamo"
: "${CONDA_PREFIX:?activate the conda environment with ROOT, Boost and clangxx 18}"

want="$(tr -d '[:space:]' < "$dir/FORK_COMMIT")"
have="$(git -C "$bdm" rev-parse HEAD)"
if [ "$have" != "$want" ]; then
  echo "abm-gen-biodynamo: the fork is at $have, FORK_COMMIT says $want" >&2
  exit 1
fi

headers=(
  core/agent/cell.h core/agent/spherical_agent.h
  neuroscience/neuron_soma.h neuroscience/neurite_element.h
  core/behavior/gene_regulation.h core/behavior/chemotaxis.h
  core/behavior/regulatory_network.h core/behavior/secretion.h
  core/behavior/stateless_behavior.h core/behavior/growth_division.h
)
args=(--cxx "$CONDA_PREFIX/bin/clang++" --std c++20 --side "$dir/side.yaml")
paths=()
for h in "${headers[@]}"; do
  args+=(--include "$h")
  paths+=("$bdm/src/$h")
done
out="$(mktemp -d)/generated.h"
# The fork's build defines these (its CMakeLists.txt); USE_BOOST brings in RegulatoryNetwork.
python3 "$here/tools/abm-gen/abm_gen.py" --out "$out" "${args[@]}" "${paths[@]}" -- \
  -DUSE_BOOST -DUSE_DICT -DLINUX -DBDM_REALT=double \
  -I"$bdm/src" -I"$bdm/third_party" -I"$CONDA_PREFIX/include"
if [ $# -ge 2 ]; then
  cp "$out" "$2"
fi
if diff -u "$dir/expected.txt" "$out"; then
  echo "abm-gen-biodynamo: the output matches expected.txt"
else
  echo "abm-gen-biodynamo: the output differs from expected.txt (above)" >&2
  exit 1
fi
