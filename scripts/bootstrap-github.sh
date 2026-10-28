#!/usr/bin/env bash
# Turns docs/ROADMAP.md into GitHub labels, milestones and issues, and applies repository settings.
# Requires the GitHub CLI (https://cli.github.com), authenticated with `gh auth login`.
# Usage: scripts/bootstrap-github.sh [--protect] [--reviews N]
#   --protect    require the "CI passed" check on main (needs a public repository or a paid plan)
#   --reviews N  also require N approving reviews (keep 0 while you work alone)
# Safe to re-run: labels are updated, existing milestones and issues are skipped.
set -euo pipefail
protect=0
reviews=0
while [ $# -gt 0 ]; do
  case "$1" in
    --protect) protect=1 ;;
    --reviews) reviews="$2"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
command -v gh >/dev/null || { echo "GitHub CLI not found: https://cli.github.com" >&2; exit 1; }
cd "$(dirname "$0")/.."
REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner)
VISIBILITY=$(gh repo view --json visibility -q .visibility)
DOCS="https://github.com/$REPO/blob/main/docs"
echo "Bootstrapping $REPO ($VISIBILITY)"

# ---------- labels
label() { gh label create "$1" --repo "$REPO" --color "$2" --description "$3" --force >/dev/null; }
label schema 1d76db "Schema, visitor protocol and generator"
label checkpoint 5319e7 "Checkpoint files"
label exchange b60205 "Exchange between processes"
label visualization 5ab4ac "Visualization files"
label adapter c2e0c6 "Simulator adapters (BioDynaMo, PhysiCell)"
label packaging fbca04 "Install, packages and releases"
label decision fef2c0 "Decision to take and record"
label research bfdadc "Research or measurement"
label security e11d21 "Security or robustness"
label performance f9d0c4 "Performance"
label run-benchmarks ff7619 "Maintainers: run benchmarks on the dedicated machine"
echo "labels: done"

# ---------- milestones
existing_milestones=$(gh api "repos/$REPO/milestones?state=all&per_page=100" -q '.[].title')
milestone() {
  if grep -Fxq -- "$1" <<<"$existing_milestones"; then echo "exists:  milestone $1"; return; fi
  gh api "repos/$REPO/milestones" -f title="$1" -f description="$2" >/dev/null
  echo "created: milestone $1"
}
M0="0 · Test bed and baselines"
M1="1 · Schema and generator"
M2="2 · Checkpoints"
M3="3 · Exchange"
M4="4 · Visualization files"
M5="5 · Adapters"
M6="6 · Release and packaging"
MD="Decisions"
milestone "$M0" "Benchmark machine and multi-node allocation; baselines recorded; CI green."
milestone "$M1" "Type and field descriptions, visitor protocol, and the abm-gen generator."
milestone "$M2" "HDF5 checkpoint files with exact restart and schema evolution."
milestone "$M3" "Library API for exchanging agents between processes; optional MPI module."
milestone "$M4" "VTKHDF output for agents and continuum fields."
milestone "$M5" "BioDynaMo and PhysiCell adapters; MultiCellDS import and export."
milestone "$M6" "Install rules, conda-forge and Spack packages, DOI per release."
milestone "$MD" "The open questions of docs/INTEGRATION.md."

# ---------- issues
existing=$(gh issue list --repo "$REPO" --state all --limit 1000 --json title -q '.[].title')
issue() {
  if grep -Fxq -- "$1" <<<"$existing"; then echo "exists:  $1"; return; fi
  gh issue create --repo "$REPO" --title "$1" --milestone "$2" --label "$3" --body "$4" >/dev/null
  echo "created: $1"
  sleep 1
}
issue "Register the benchmark machine and record baselines" "$M0" "performance" \
"Follow docs/QUALITY-GATES.md: register a self-hosted runner labelled benchmark, then record bench/baseline/<host>.json."
issue "Prepare a multi-node test allocation" "$M0" "performance" \
"Adapt scripts/hpc/multinode.sbatch to a site and enable hpc.yml."
issue "Type identifiers: name hash or registry" "$MD" "decision" \
"Open question 1 of $DOCS/INTEGRATION.md."
issue "Parallel checkpoints: shared file, one file per process, or both" "$MD" "decision" \
"Open question 2 of $DOCS/INTEGRATION.md."
issue "Generator: language and location" "$MD" "decision" \
"Open questions 3 and 4 of $DOCS/INTEGRATION.md."
issue "Units: metadata only or conversion" "$MD" "decision" \
"Open question 5 of $DOCS/INTEGRATION.md."
issue "Schema headers: type and field descriptions" "$M1" "schema" \
"Types, fields, kinds, units and roles as specified in $DOCS/INTEGRATION.md."
issue "Visitor protocol, and the existing codecs on top of it" "$M1" "schema" \
"Packing, unpacking, writing, reading and comparison implemented as visitors."
issue "abm-gen: generate visitor functions from C++ headers" "$M1" "schema" \
"Reads simulator headers with Clang's parser and emits one visitor function per type."
issue "Validate abm-gen on BioDynaMo's agent and behaviour classes" "$M1" "schema,adapter" \
"Generated descriptions must match hand-checked ones for every class of the test models."
issue "HDF5 checkpoint writer and reader" "$M2" "checkpoint" \
"The layout of $DOCS/INTEGRATION.md, with an exact-restart test."
issue "Parallel checkpoints" "$M2" "checkpoint,performance" \
"Collective writing with parallel HDF5, and one file per process plus an index."
issue "Schema evolution in checkpoints" "$M2" "checkpoint" \
"Files written with an older schema version load: missing fields take defaults, unknown fields are skipped."
issue "Exchange API on top of the schema" "$M3" "exchange" \
"Packing and unpacking of agents described by the schema, into receiver-owned memory."
issue "Delta encoding per neighbour" "$M3" "exchange,performance" \
"Encode each frame as the difference from the previous frame sent to the same neighbour."
issue "Optional MPI module" "$M3" "exchange" \
"Send and receive exchange buffers on a communicator supplied by the simulator."
issue "VTKHDF point data for agents" "$M4" "visualization" \
"Positions, sizes and every field with the export role; opens in ParaView and PyVista."
issue "VTKHDF image data for continuum fields" "$M4" "visualization" \
"Substance concentrations and other grids, with origin, spacing and unit."
issue "BioDynaMo adapter: checkpoint and restart" "$M5" "adapter" \
"Generated visitors for BioDynaMo models; a run continued from a checkpoint matches the uninterrupted run."
issue "BioDynaMo adapter: exchange between processes" "$M5" "adapter,exchange" \
"Use the exchange API for agents near the borders between processes."
issue "PhysiCell adapter: checkpoint and restart" "$M5" "adapter" \
"Hand-written description of Cell, its phenotype and custom data."
issue "MultiCellDS import and export" "$M5" "adapter" \
"Move initial conditions and results between simulators through MultiCellDS."
issue "Boundary-behaviour consistency tests" "$M5" "research" \
"Extend tests/physics so simulators can check behaviour at walls and grid edges (docs/FINDINGS.md)."
issue "Install rules and CMake package" "$M6" "packaging" \
"Make find_package(abmio) work for an installed library; test it with a small external project in CI."
issue "conda-forge package" "$M6" "packaging" \
"Submit a recipe to conda-forge's staged-recipes."
issue "Spack package" "$M6" "packaging" \
"Add a package to Spack for HPC clusters."
issue "DOI for every release" "$M6" "packaging" \
"Connect the repository to Zenodo and keep CITATION.cff current."
echo "issues: done"

# ---------- repository settings
vars=$(gh variable list --repo "$REPO" --json name -q '.[].name')
setvar() { gh variable set "$1" --repo "$REPO" --body "$2" >/dev/null; echo "variable: $1=$2"; }
if [ "$VISIBILITY" = PUBLIC ]; then setvar PUBLIC_REPO true; else setvar PUBLIC_REPO false; fi
grep -Fxq HAS_BENCHMARK_RUNNER <<<"$vars" || setvar HAS_BENCHMARK_RUNNER false
grep -Fxq HAS_HPC_RUNNER <<<"$vars" || setvar HAS_HPC_RUNNER false

gh api -X PUT "repos/$REPO/actions/permissions/workflow" -f default_workflow_permissions=read \
  -F can_approve_pull_request_reviews=false >/dev/null && echo "GITHUB_TOKEN default: read-only"
gh api -X PUT "repos/$REPO/vulnerability-alerts" >/dev/null 2>&1 && echo "Dependabot alerts: on" \
  || echo "note: could not enable Dependabot alerts"
gh api -X PUT "repos/$REPO/automated-security-fixes" >/dev/null 2>&1 && echo "Dependabot security updates: on" \
  || echo "note: could not enable Dependabot security updates"
if [ "$VISIBILITY" = PUBLIC ]; then
  gh api -X PUT "repos/$REPO/private-vulnerability-reporting" >/dev/null 2>&1 \
    && echo "private vulnerability reporting: on" || echo "note: could not enable private vulnerability reporting"
fi
if [ "$protect" -eq 1 ]; then
  if [ "$reviews" -gt 0 ]; then
    review_rule="{\"required_approving_review_count\": $reviews, \"require_code_owner_reviews\": true}"
  else
    review_rule=null
  fi
  printf '{"required_status_checks": {"strict": true, "contexts": ["CI passed"]}, "enforce_admins": false, "required_pull_request_reviews": %s, "restrictions": null, "allow_force_pushes": false, "allow_deletions": false}\n' "$review_rule" \
    | gh api -X PUT "repos/$REPO/branches/main/protection" --input - >/dev/null \
    && echo "main: requires 'CI passed'" \
    || echo "note: branch protection needs a public repository or a paid plan"
fi
echo "Done. Roadmap: https://github.com/$REPO/milestones"
