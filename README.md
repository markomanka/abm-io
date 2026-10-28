# abm-io

[![CI](https://github.com/markomanka/abm-io/actions/workflows/ci.yml/badge.svg)](https://github.com/markomanka/abm-io/actions/workflows/ci.yml)
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.23731697.svg)](https://doi.org/10.5281/zenodo.23731697)

Checkpoints, exchange of agents between processes, and files for visualization, for agent-based simulators of
multicellular biology. abm-io is built for BioDynaMo and PhysiCell first, and is meant to be adopted by other
simulators as well.

**Status:** early development (0.1). The exchange codecs work, are tested and fuzzed; the schema, checkpoint
files, visualization files and the adapters are being built. The API and the file formats will change before 1.0.

## Why

Simulators of multicellular systems share needs that lie outside their core algorithms:

- **Checkpoint and restart**, so long simulations can stop and resume exactly. A community benchmark of
  cell-based simulation tools [1] lists neither BioDynaMo nor PhysiCell as supporting it.
- **Consistent, self-describing output**, so results can be compared across tools, as the same benchmark
  recommends, and opened by standard tools.
- **Efficient exchange of agents** when a simulation runs on many processes. TeraAgent [2] showed that
  serialization tailored to the agent model, and delta encoding that exploits how little agents change between
  iterations, make simulations of half a trillion agents possible.

abm-io provides these once, in one library, behind a thin adapter per simulator. It follows principles
published in [1] and [2] and shares no code with the tools described there. `docs/INTEGRATION.md` specifies how
simulators connect.

## What is here

| Path | Content |
| --- | --- |
| `include/abmio/`, `src/` | The library: codecs (rows, columns, delta encoding, compression) and exchange with receiver-owned memory |
| `tests/` | Unit, corruption, codec, physics and MPI tests |
| `fuzz/` | Fuzz harnesses for every decoder |
| `bench/` | Benchmarks and baselines |
| `docs/INTEGRATION.md` | The integration contract: schema, checkpoint files, exchange, visualization files |
| `docs/ROADMAP.md` | Milestones and how progress is tracked |
| `docs/FINDINGS.md` | Measurements and observations behind the design |
| `docs/QUALITY-GATES.md` | The checks every change passes, and how to add the benchmark machine |

## Build and test

With conda-forge, on any platform (the route BioDynaMo uses):

```bash
conda env create -f environment.yml     # or mamba / micromamba
conda activate abm-io
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

With system packages instead, install the dependencies, then run the same three commands:

```bash
sudo apt-get install cmake ninja-build pkg-config liblz4-dev libgtest-dev   # Ubuntu
brew install cmake ninja lz4 googletest pkgconf                            # macOS (Homebrew)
```

`CONTRIBUTING.md` covers sanitizer, fuzzing and MPI builds.

## Contributing and security

Contributions are welcome; see `CONTRIBUTING.md`. Report vulnerabilities privately, as described in
`SECURITY.md`.

## Citing

Cite abm-io with the DOI https://doi.org/10.5281/zenodo.23731697, which always resolves to the latest version. Each
release also has its own DOI; version 0.1.0 is https://doi.org/10.5281/zenodo.23731698. `CITATION.cff` has the details.

## Licence

Apache-2.0, see `LICENSE`.

## References

1. Ntiniakou T, Osborne J, Zhao J, et al. *Open Benchmarking for Cell-Based Multiscale Models: Lessons from a
   Community Initiative.* bioRxiv 2025.07.16.664358 (2025). https://doi.org/10.1101/2025.07.16.664358
2. Breitwieser L, Hesam A, Yağlıkçı AG, Sadrosadati M, Rademakers F, Mutlu O. *TeraAgent: A Distributed
   Agent-Based Simulation Engine for Simulating Half a Trillion Agents.* arXiv:2509.24063 (2026); accepted
   for publication in IEEE Access. https://arxiv.org/abs/2509.24063
