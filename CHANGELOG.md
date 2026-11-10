# Changelog

What each version delivered, the decisions it rests on, and the measurements behind them. Decisions are
numbered as in [docs/RATIONALE.md](docs/RATIONALE.md); measurements are the sections of
[docs/FINDINGS.md](docs/FINDINGS.md). The plan for the numbers themselves is in
[docs/VERSIONS.md](docs/VERSIONS.md).

## Unreleased

### Decisions

12 is decided: the checkpoint container is written against HDF5's C API, not HighFive. The rule written
before the comparison failed HighFive on two clauses, packaging (Homebrew has no formula) and capability
(three C calls where one escape hatch was allowed).

## 0.2.0 — 8 November 2026

DOI [10.5281/zenodo.24056736](https://doi.org/10.5281/zenodo.24056736).

The model a simulator is described with, and the groundwork for the checkpoint container. No container
yet: nothing here writes a file a simulator could restore from.

### Added

- **Type and field descriptions, with roles, units and identifiers**, and the visitor protocol they drive.
  A field is described once and then read, written, exchanged and checked by the same declaration.
- **`abm-gen`**, which writes those descriptions from a simulator's C++ headers, so a model's types are
  described without anyone maintaining a second copy of them by hand.
- **`FieldTraits`**, the customisation point by which a simulator declares its own member types once.
- **Five record types**: the simulation's step and time, one row per random stream, what wrote the
  checkpoint, what the run depended on, and what changed since. Tables and records are the same mechanism,
  so a simulation's own state versions and resumes by the same rules as its agents.
- **Provenance and lineage.** A checkpoint records its dependencies with a digest and whether each affects
  results; a resume across a changed dependency is refused unless the component is named, and every
  acceptance is written down and carried by later checkpoints.
- **The LZ4 filter for HDF5**, registered by abm-io itself with the LZ4 it already links, so its files
  compress without abm-io needing a plugin and remain readable by `h5py` with `hdf5plugin`.
- **The compression policy**: `off`, `on` or `auto`, with `auto` deciding from declared inputs rather than
  from live timings, so two runs of the same simulation write the same bytes.
- **The Rust decoder experiment** (milestone R): the delta decoder in safe Rust behind a C interface, off
  by default, with nine Kani proofs and a differential comparison against the C++ decoder.
- **Four design documents**: the generator, the rationale, learned operations and live data, and storage
  backends.

### Changed

- Resuming a checkpoint is **strict**: a schema that differs in anything affecting mechanics fails the
  load and says what differs. Reading an old file for analysis stays permissive.
- Checkpoint files are readable by any HDF5 tool **that has the standard filter plugins**, rather than by
  any HDF5 tool at all. The speed of LZ4 was judged worth the narrower promise.

### Decisions

1, the schema is read from C++ rather than the C++ written from the schema; 2, units are recorded and
never converted; 3, schema evolution is strict when resuming; 4, one file per process with parallel HDF5
optional; 5, fixed-size and variable-length data travel separately; 6, the Rust experiment is bounded and
ends in a decision; 7, an event stream is an option for delta level 3; 8, checkpoints compress with LZ4
rather than gzip; 9, whether to compress is decided from declared inputs; 10, a checkpoint becomes visible
only when it is complete; 11, a changed dependency stops a resume and may be let through by name; 12, the
rule that decides between HighFive and HDF5's C API.

### Measurements

Exchange performance against the gather bound; robustness under fuzzing and sanitizers; behaviour at
boundaries; checkpoint compression before the container was written, including that `h5py` with
`hdf5plugin` reads what we write; where the compression crossover lies; what a reader sees when a
checkpoint is interrupted, where a hundred kills left a naive layout usable 15 times out of 100 and the
manifest layout 100 times out of 100; and the HighFive-against-C-API comparison on two machines.

### Known limitations

- No checkpoint container: the layout, the policy and the rules exist, the writer and reader do not.
- `abm-gen` has not been validated against BioDynaMo's own classes, only against fixtures that mimic them.
- The benchmarks run on shared runners, so the 5 % threshold of `docs/QUALITY-GATES.md` cannot yet be
  resolved; `bench` is skipped until a dedicated machine exists.
- Milestone R is undecided: the Rust decoder is built only when asked for and ships nothing.

## 0.1.0 — 28 October 2026

The first public version: the exchange prototype with its codecs, the test bed, and the quality gates that
every change has passed since. DOI 10.5281/zenodo.23731697.
