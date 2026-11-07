# Findings

Measurements and observations that shaped the design in `docs/INTEGRATION.md`.

## Exchange performance

Measured with the benchmark in `bench/`, on one core of a 2.8 GHz Xeon, for 20,000 to 1,000,000 synthetic
agents shaped like typical cells (position, diameter, several scalar fields and behaviours):

| Operation | Time per agent |
| --- | --- |
| Gather bound: reading the fields involved, the minimum any encoder pays | 27–55 ns |
| Packing into rows | 38–67 ns |
| Receiving into reused, receiver-owned memory | 17–46 ns |

Packing therefore stays within 1.25–1.55× of the gather bound. The library's target is 1.6×.

**Delta encoding.** Successive iterations change agents only slightly. Encoding each frame as the difference from
the previous one (XOR of consecutive frames, byte shuffling, then LZ4) brings the steady-state exchange down to
about 25 bytes per agent in the benchmark scenario, measured at iteration 25. The idea of exploiting the
iterative nature of agent-based simulations follows TeraAgent [2]; the method here is independent.

## Robustness

Messages between processes, and later files, must be treated as untrusted input. Every decoder checks counts,
offsets, type identifiers and indices before using them, and rejects malformed input without reading or writing
outside the buffer. Corruption tests and fuzzing under AddressSanitizer and UBSan cover every decoder.

## Behaviour at boundaries

Results only compare across simulators if boundaries behave physically. The physics tests in `tests/physics`
reproduce four effects that make agents accumulate at borders:

- **Clamping positions at closed walls** concentrates agents on the wall: 2.7 times the expected density in
  the test. Reflection plus a contact force at the wall removes the effect, at no measurable cost.
- **Chemotaxis at full speed along any normalized gradient**, however small, puts 41% of the agents in the
  first layer even when the gradient is flat. Scaling the speed with the gradient's magnitude,
  speed × |∇c| / (K + |∇c|), avoids it.
- **One-sided gradients at the edge of a grid** that ignore the boundary condition bias movement near the
  walls; ghost cells fix it.
- **A grid that grows with the colony** in open space makes the colony's edge a boundary; padding the grid fixes
  it.

These match the community benchmark's observation [1] that differences between simulators often come from
modelling and numerical choices rather than from the biology.

## Dependencies

Everything abm-io and its users need is packaged by community-maintained ecosystems. On conda-forge, for Linux
and macOS (October 2026): HDF5 2.2.0, HighFive 3.3.0, nlohmann_json 3.12.0, LZ4, GoogleTest, and ParaView 6.2.0
for viewing VTKHDF files. Spack packages the same libraries for HPC clusters, and also Catalyst 2
(`libcatalyst`) for in-situ visualization.

## Checkpoint compression, before the container was written

Tested with HDF5 1.10.10 and liblz4 1.9.4 on Linux, to settle how checkpoints would be compressed before any
container code existed:

| Question | Answer |
| --- | --- |
| Can a dataset declare the registered LZ4 filter (32004) without it being installed? | No. With a mandatory filter, dataset creation fails: "required filter 32004 is not registered" |
| Can abm-io provide the filter itself? | Yes, with `H5Zregister` and the LZ4 it already links; `H5Zfilter_avail` then reports it available |
| Does a round trip through HDF5 preserve the data? | Yes, byte for byte, 8192 doubles |
| How well does it compress? | 65,536 bytes of data to a 7,113-byte file, with 1,024-element chunks |
| Can a third party read it? | Yes: `h5py` with `hdf5plugin` returns identical values and reports the filter as 32004 |

So the compression decision costs abm-io no new dependency and keeps the files readable by the usual Python
tools. The filter's format, which had to be matched exactly for that last row to hold, is the total decoded
size as a big-endian 64-bit integer, the block size as a big-endian 32-bit integer, then each block as its
big-endian 32-bit compressed size followed by its bytes.

The direct chunk write (`H5Dwrite_chunk`) was tried first and works, but it does not avoid registering the
filter, so it was dropped in favour of HDF5's ordinary paths.

## Where the compression crossover lies

The arithmetic behind the compression policy, from one measured number and two that are not yet measured
here and must be before the defaults are trusted:

| Quantity | Value | Source |
| --- | --- | --- |
| Gather bound, reading the fields of an agent | 27–55 ns | measured, see above |
| Checkpointed state per agent | about 120 bytes | fifteen fields of a cell-like agent |
| LZ4 compression rate, one core | about 500 MB/s | typical, to measure on the target machine |
| LZ4 decompression rate, one core | several GB/s | typical, to measure |

For a million agents: the gather costs about 55 ms, compressing the resulting 120 MB about 240 ms, and
writing the compressed 15 MB almost nothing. Uncompressed, the same checkpoint costs the gather plus
writing 120 MB. Compression therefore pays when the storage is slower than roughly a gigabyte per second,
and costs about a factor of two on faster storage. Restoring is the easier direction, since LZ4
decompresses several times faster than it compresses, and is bound by scattering values back into agents.

**What writing the properties found, before any container existed.** Two faults, neither of which a test of
the decision's output would have caught:

- the comparison in the rule was inverted, so the policy would have compressed exactly when it should not.
  A test comparing the integer rule against the timing it was derived from caught it;
- an unknown compression ratio leaned towards not compressing while every other unknown leaned towards
  compressing. Since zero sits at the bottom of an ordered field, the monotonicity property rejected it
  immediately. All unknowns now lean the same way.
## What a reader sees when a checkpoint is interrupted

A writer killed in the middle of a checkpoint is the normal case on a busy cluster or on preemptible cloud
instances, not an edge case. Two layouts were compared by killing a writer at a random moment, a hundred
times each, and asking what a reader then observes. Sixteen ranks, 150,000 agents each, five fields: about
93 MB and 0.12 s per checkpoint, with kills uniformly inside that window.

| Writer | Complete | Partial | Corrupt |
| --- | --- | --- | --- |
| Pointer moved first, each rank written straight to its final name | 15 | 81 | 4 |
| Temporary names renamed, manifest written last, pointer replaced last | **100** | 0 | 0 |

*Complete* means the reader restored a whole checkpoint, *partial* that the pointer named a checkpoint whose
parts were not all there, and *corrupt* that a file was listed and could not be opened at all.

The four corrupt cases are the ones that matter. A partial checkpoint announces itself; a corrupt one is
discovered only when a job has already queued, started and taken its nodes. With the manifest, the reader
always saw a complete checkpoint, falling back to the previous one when the new one had not finished: 100
times out of 100, where the naive layout managed 15.

The mechanism is ordinary and cheap. Renaming within a directory is atomic on POSIX file systems, so a name
either refers to the old content or to the new one. Writing the parts under temporary names, then the
manifest, then the pointer, makes that atomicity cover the whole checkpoint rather than one file.

## References

1. Ntiniakou T, Osborne J, Zhao J, et al. *Open Benchmarking for Cell-Based Multiscale Models: Lessons from a
   Community Initiative.* bioRxiv 2025.07.16.664358 (2025). https://doi.org/10.1101/2025.07.16.664358
2. Breitwieser L, Hesam A, Yağlıkçı AG, Sadrosadati M, Rademakers F, Mutlu O. *TeraAgent: A Distributed
   Agent-Based Simulation Engine for Simulating Half a Trillion Agents.* arXiv:2509.24063 (2026); accepted
   for publication in IEEE Access. https://arxiv.org/abs/2509.24063
