# Roadmap

abm-io aims to give agent-based simulators of multicellular biology, starting with BioDynaMo and PhysiCell,
checkpoint and restart, efficient exchange of agents between processes, and self-describing files for
visualization, without slowing them down. `docs/INTEGRATION.md` specifies the design.

Each milestone below is a GitHub milestone with one issue per piece of work. A milestone is done when its
issues are closed and its *done when* check passes.

| Milestone | Work | Done when |
| --- | --- | --- |
| 0 · Test bed and baselines | CI on every change (build matrix, sanitizers, fuzzing, MPI, coverage, static analysis); a dedicated benchmark machine; a multi-node allocation | Baselines recorded on the benchmark machine; CI green |
| 1 · Schema and generator | Type and field descriptions with roles; the visitor protocol; `abm-gen`, which generates visitor functions from C++ headers; validation on BioDynaMo's agent and behaviour classes | Generated descriptions match hand-checked ones for every class of the test models |
| 2 · Checkpoints | HDF5 checkpoint files: writer, reader, schema evolution, parallel writing | Save and restore reproduce the state exactly; files from older schema versions load |
| 3 · Exchange | The library API for exchanging agents, built on the schema; delta encoding per neighbour; an optional MPI module | Multi-process tests pass; packing stays within 1.6× of the gather bound |
| 4 · Visualization files | VTKHDF output for agents (point data) and continuum fields (image data) | Files open in ParaView and PyVista and match the simulation state |
| 5 · Adapters | BioDynaMo adapter (generated) and PhysiCell adapter (hand-written); MultiCellDS import and export; boundary-behaviour consistency tests | In each simulator, a run continued from a checkpoint matches the uninterrupted run |
| 6 · Release and packaging | Install rules and CMake package; conda-forge and Spack packages; a DOI per release | Installable from conda-forge and Spack; release 1.0 |
| Decisions | The open questions of `docs/INTEGRATION.md` | Each decision recorded in its issue |

Milestones 1 to 4 can overlap; milestone 5 needs 1 and 2 for checkpoints, and 3 for exchange.

## Principles

- **Performance is a requirement.** Every change runs the benchmarks on a dedicated machine, and a slowdown
  beyond the agreed tolerance blocks it.
- **Untrusted input is expected.** Every decoder validates its input and is fuzzed.
- **Formats are documented and versioned.** Files carry their schema, and readers keep supporting older versions.
- **Simulators keep their models.** Adapters describe existing types; models need no changes.

## How progress is tracked

`scripts/bootstrap-github.sh` creates the labels, milestones and issues. It is safe to re-run: existing
milestones and issues are skipped. When the plan changes, update this file, the script and the matching issues
in the same pull request.
