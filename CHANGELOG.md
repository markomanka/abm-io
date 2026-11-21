# Changelog

What each version delivered, the decisions it rests on, and the measurements behind them. Decisions are
numbered as in [docs/RATIONALE.md](docs/RATIONALE.md); measurements are the sections of
[docs/FINDINGS.md](docs/FINDINGS.md). The plan for the numbers themselves is in
[docs/VERSIONS.md](docs/VERSIONS.md).

## Unreleased

## 0.3.0 — 21 November 2026

DOI [10.5281/zenodo.24421952](https://doi.org/10.5281/zenodo.24421952).

Checkpoints: a simulation's state can be saved and restored exactly. Tables of agents and records are
written and read a block at a time through the visitor protocol, a checkpoint becomes visible only when it
is complete, and a resume stops when the model or what the run depended on has changed in a way that
affects results.

### Added

- The checkpoint container's storage layer (`src/container/`, internal): typed, extendable columns in HDF5
  through the C API, appended block by block and read whole or by range, with strings and attributes.
- `CheckpointWriter` (`abmio/checkpoint.h`): objects of a described type, walked through the visitor
  protocol a block at a time, become a table of columns; a record is a table with one row. The file carries
  the schema of what it holds, as JSON, and each table its type, its types' versions and its fields'
  units. Variable-size arrays are written as `values` and `offsets` (n + 1 entries from 0), as in Apache
  Arrow (decision 13), both from `std::vector` and from a simulator's own container through `FieldTraits`
  (`Size`, `At`, `Resize`).
- `CheckpointReader`: the reverse of the writer, a block at a time, restoring every field with the
  checkpoint role. Offsets are checked before use (from 0, never decreasing, ending at the number of
  values), and booleans are read so that a damaged byte cannot make an invalid `bool`.
- The strict resume (decision 11): a table loads only if its fields match the model's in name, kind,
  element type, count and unit, and every type of its chain has the model's version, and the failure names
  what differs. `CheckpointReader::CheckResume` compares the components a checkpoint records with those
  present now: every change that affects results stops the resume, all of them named, unless the caller
  accepts that component by its exact name, saying who and why; each accepted change becomes a lineage
  row, carried by every later checkpoint.
- Visibility (decision 10; `abmio/store.h`, `abmio/manifest.h`): each part is written as `….tmp` and
  renamed when closed, after flushing to disk; `PrepareCheckpoint`, `CommitCheckpoint` and `FindLatest`
  keep a directory of checkpoints in which the manifest and then the pointer `latest` are replaced by
  rename, so a reader sees the previous complete checkpoint or the new one, never a mixture. The manifest
  and pointer are JSON of a fixed shape, read by a strict parser that is fuzzed (`fuzz/fuzz_manifest.cc`)
  and refuses names that could leave the directory.
- `ToJson(const Schema&)`, the form in which a file carries its schema.
- The acceptance test of milestone 2 (`tests/checkpoint_acceptance_test.cc`): a small simulation saved in
  two parts, restored with the strict resume and continued, ends bit for bit in the state of the same run
  without stopping; a checkpoint killed before its commit leaves the previous one to restore from.

### Changed

- CI follows the platform policy shared with the BioDynaMo fork (`docs/QUALITY-GATES.md`, "Platforms"):
  Ubuntu 24.04 and 26.04 and macOS 15 on every pull request, macOS 26 weekly; Ubuntu 22.04 is no longer
  tested.

### Fixed

- Files are the same bytes for the same data on every build: no group records times, and every HDF5
  writes 1.8's object-header format.

### Decisions

13 is decided: variable-size fields are stored as values and offsets, as Apache Arrow does, not with HDF5's
variable-length type, whose elements HDF5 cannot compress and which would break chunk-bounded writing.

12 is decided: the checkpoint container is written against HDF5's C API, not HighFive. The rule written
before the comparison failed HighFive on two clauses, packaging (Homebrew has no formula) and capability
(three C calls where one escape hatch was allowed).

10 and 11, decided earlier, are implemented: the visibility rule and the strict resume with acceptance by
name.

### Measurements

None new. The layout rests on those of 0.2.0: compression, the crossover, the interrupted checkpoints
(100 out of 100 usable with the manifest layout) and the binding comparison.

### Known limitations

- Nested records, lists of behaviours and `StoredAs` fields are refused with the field named (#72);
  BioDynaMo's behaviours need them before its adapter can restore a simulation.
- Resuming is strict only: reading an older file with defaults for missing fields is schema evolution
  (#13).
- Parallel checkpoints write one file per writer, but how processes agree before the commit is not yet
  provided (#12).
- The store relies on POSIX rename; object stores need another mechanism (`docs/STORAGE-BACKENDS.md`).
- The benchmarks still run on shared runners, and milestone R is undecided, as in 0.2.0.

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
