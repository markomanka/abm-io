# Versions: what the numbers mean, and the road to 1.0.0

abm-io follows [semantic versioning](https://semver.org). Before 1.0.0 the formats and the API may still
change; after it they may not, except as the rules below allow. This file says what each number will have
delivered, what 1.0.0 promises, and what to do when a version is cut.

## Before 1.0.0

A minor version marks a capability this repository delivers and tests. It does not wait on another
project: validating against a real simulator is part of what 1.0.0 means, not of each step towards it.

| Version | What it delivers | Milestone | State |
| --- | --- | --- | --- |
| 0.1.0 | The first public version: the exchange prototype, the codecs, the test bed, the quality gates | 0 | released 28 October 2026 |
| 0.2.0 | The model: type and field descriptions with roles, identifiers, the visitor protocol, `abm-gen`, the five record types with provenance and lineage, the LZ4 filter registered with HDF5, and the Rust decoder experiment with its proofs | 1 | this release |
| 0.3.0 | Checkpoints: the writer and the reader, the layout of `docs/INTEGRATION.md`, chunk-bounded memory, the manifest and the visibility rule, the strict resume | 2 | next |
| 0.4.0 | Exchange as a library API on the schema, with delta encoding per neighbour | 3 | |
| 0.5.0 | Visualization files: VTKHDF for agents and continuum fields | 4 | |
| 0.6.0 | Adapters: BioDynaMo through `abm-gen`, PhysiCell by hand, MultiCellDS in and out | 5 | |
| 0.7.0 | Packaging: install rules, a CMake package, conda-forge and Spack | 6 | |

A patch version fixes something without changing a format or an interface.

**Formats may change before 1.0.0**, and when one does, its version number in the file or the message is
raised and the change is recorded in `CHANGELOG.md` and in `docs/INTEGRATION.md`. A reader of an older
version says so rather than guessing.

## What 1.0.0 promises

Not a date and not a feature count. Five things, each of which can be checked:

1. **A simulator uses it in earnest.** BioDynaMo saves and restores a real simulation through abm-io, in a
   released version of BioDynaMo, and a second simulator reads those files.
2. **The file format is frozen.** Any 1.x reader reads any 1.y file. Fields may be added; nothing is
   removed or reinterpreted without a 2.0.0.
3. **The API is frozen** in the same sense: additions only within 1.x.
4. **Every decision stands or is marked superseded.** `docs/RATIONALE.md` carries no entry still described
   as open, and the Rust experiment of milestone R has ended in a recorded decision either way.
5. **The quality gates run where they mean something**: the benchmarks on a dedicated machine against
   recorded baselines, not on a shared runner.

Until all five hold, the version stays below 1.0.0 however complete the library looks.

## Cutting a version

The numbers appear in four places and must agree. The checklist:

1. `CMakeLists.txt`: `project(abmio VERSION x.y.z …)`.
2. `CITATION.cff`: `version` and `date-released`.
3. `CHANGELOG.md`: a section for the version, dated, listing what changed, the decisions it rests on by
   their entry in `docs/RATIONALE.md`, and the measurements behind them by their section in
   `docs/FINDINGS.md`. This section is the release's description, and is what Zenodo preserves.
4. `.zenodo.json`: nothing version-specific, but check the authors and the related identifiers.

Then tag `vx.y.z` on `main`, create the GitHub release with the changelog section as its body, and let the
Zenodo integration mint the DOI. `CITATION.cff` keeps the concept DOI, which resolves to the newest
version; a release's own DOI appears on Zenodo afterwards.

Every check must have passed on the tagged commit, by the rule in `docs/QUALITY-GATES.md`: `skipped` and
`neutral` are acceptable, `cancelled` is not.
