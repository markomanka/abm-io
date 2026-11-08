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
| LZ4 compression rate, one core | about 500 MB/s assumed; 0.8–1.0 GB/s measured through HDF5's filter pipeline | the binding comparison below: the LZ4 write of 44 MB took 44 ms longer than the uncompressed one in the sandbox and 53 ms longer on the laptop |
| LZ4 decompression rate, one core | several GB/s | typical, to measure |

For a million agents: the gather costs about 55 ms, compressing the resulting 120 MB about 240 ms, and
writing the compressed 15 MB almost nothing. Uncompressed, the same checkpoint costs the gather plus
writing 120 MB. Compression therefore pays when the storage is slower than roughly a gigabyte per second,
and costs about a factor of two on faster storage; the binding comparison below measured that factor on
two machines, 2.0 and 1.8, with the compressor faster than assumed, so the crossover moves up with it and
the rule stands. Restoring is the easier direction, since LZ4
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

What this does not cover, and why the contract states a property rather than this mechanism, is in
[STORAGE-BACKENDS.md](STORAGE-BACKENDS.md), together with the two experiments still outstanding.

The mechanism is ordinary and cheap. Renaming within a directory is atomic on POSIX file systems:
a reader opening the name finds either the old content or the new one, never a mixture. Writing the parts under temporary names, then the
manifest, then the pointer, makes that atomicity cover the whole checkpoint rather than one file.

## HighFive or the C API: the comparison, first run

The rule is `docs/RATIONALE.md` entry 12, written before this ran. The harness is `bench/h5binding/`: the
same checkpoint writer and reader on the C API and on HighFive 3.3.0 behind one interface, five agent
columns written one chunk at a time into extendable datasets, a record and two tables with variable-length
strings, read back block by block and checked against the generator. `abmio_h5binding_compare` times each
phase in a child process and reports its peak resident size; `compare.py` runs it and then `h5diff`,
`h5dump -pH`, `cmp` and `h5py` on the files. Run here on a shared sandbox (Ubuntu 24.04, HDF5 1.10.10,
GCC 13), which settles the capability and file questions and nothing about speed.

**1. What each can do without the C API.** Of the four things the rule names, HighFive does three natively
(the chunk cache through `Caching`, variable-length strings through `std::vector<std::string>`, chunk-bounded
writing through `resize` and `select(...).write_raw`) and the LZ4 filter through one escape hatch: it has
properties for gzip, szip and shuffle but none for a filter by identifier, so the filter is a class of our
own with `apply(hid_t)` around `H5Pset_filter`, which `DataSetCreateProps::add` accepts. That is the hatch
HighFive documents, and it passes the rule as written.

The measurement then found two more C calls the rule had not listed, both in `bench/h5binding/highfive.cc`,
marked where they occur. **Fill time**: HDF5 leaves the unwritten part of a partial last chunk
uninitialised unless told to fill on allocation, so the same agents gave files that differed in 17 bytes
from one run to the next, in bytes nobody wrote; `H5Pset_fill_time(H5D_FILL_TIME_ALLOC)` fixes it, and
HighFive has `AllocationTime` but no property for the fill time. This is a finding about the container
rather than the binding, since the C API needs the same call and a checkpoint must be reproducible byte for
byte. **Listing a dataset's filters on reading**: HighFive reports gzip and shuffle as properties, not an
arbitrary filter's identifier, so the test that checks the filter went in uses `H5Pget_filter2` on the
`hid_t` HighFive exposes. Counting everything, HighFive needed three C calls where the rule allowed for one
and named four capabilities; whether the two it did not name count against HighFive is for the decision to
say, and it is recorded here so that it can.

**2. Whether the files are the same.** Byte for byte identical, compressed and uncompressed, once both
fill on allocation: `cmp` finds no difference, `h5diff -c` no difference in contents or properties,
`h5dump -pH` the same structure and layout. Each binding reads the other's files. `h5py` with `hdf5plugin`
reads all four, reports the filter as 32004, the chunks as declared and `affects_results` as booleans
(both bindings write the enumeration h5py uses, HighFive natively, the C API by eight lines). The HDF5
command-line tools decode the LZ4 datasets only with the plugin on `HDF5_PLUGIN_PATH`, which `hdf5plugin`
ships; without it `h5diff` exits 2 on these files. One more thing fell out: `h5diff` finds the compressed and
the uncompressed file of the same binding identical in contents, which is the compression invariant of
`INTEGRATION.md` observed at the file level.

**3. Throughput and peak memory, indicative only.** A million agents, 44 bytes each, chunks of 16,384:

| Phase | C API | HighFive |
| --- | --- | --- |
| Write, uncompressed | 1,084 MB/s | 1,240 MB/s |
| Write, LZ4 | 519 MB/s | 543 MB/s |
| Read, uncompressed | 1,108–1,712 MB/s | 1,465–1,701 MB/s |
| Read, LZ4 | 804–819 MB/s | 746–788 MB/s |
| Peak resident size, any phase | 15–17 MiB | 15–17 MiB |

The differences are inside the run-to-run noise of a shared machine; the rule's thresholds (5 % and 10 %)
wait for the dedicated machine. Two things do hold whatever the noise. **The chunk bounds the memory:** at
ten million agents the files are 497 MB and 126 MB and the peak stays at 17–18 MiB, as the contract
requires. **LZ4 costs a factor of two on fast local storage**, 519 against 1,084 MB/s, which is what the
compression policy's arithmetic predicted and the reason `auto` exists.

**4. Build and packaging cost.** HighFive 3.3.0 is on conda-forge (six platforms) and in Spack; it is not
in Ubuntu 24.04's archive, so the Ubuntu jobs of the CI build only the C side and the conda jobs build both;
Homebrew was not checked from where this ran. It supports HDF5 2.x, which conda-forge ships, and 1.10, which
Ubuntu does. The HighFive implementation is 252 lines of code against 415 (blank lines and comments excluded), forty per cent fewer, over the
rule's one third; its translation unit compiles in 4.6 s against 1.6 s, the price of a header-only template
library, paid once per file that includes it. One C API detail the comparison surfaced: `H5Dvlen_reclaim`,
which frees the strings HDF5 allocates on reading, was renamed `H5Treclaim` in 1.12 and the old name is
gone from 2.x, so the C side carries a version check that HighFive hides.

**Second run, on a laptop.** The same build on an i9-12900HK under Ubuntu 26.04 (20 threads, `powersave`
governor, nothing else running, files on a tmpfs, chosen so that no disk sits between the bindings), from
conda-forge packages: HDF5 2.2.0, HighFive 3.3.0, h5py 3.16 with hdf5plugin 7.1. Everything above held
again: all tests pass on HDF5 2.x, the files are byte for byte identical, h5py reads all four, each binding
reads the other's. Five runs of a million agents, each binding reading its own file; medians, with the
spread between the fastest and the slowest run of the five. **The rates are to memory, not to a disk:**
they measure the binding and HDF5, and say nothing about the laptop's storage.

| Phase | C API | HighFive | HighFive against C API | Spread within a binding |
| --- | --- | --- | --- | --- |
| Write, uncompressed | 69 ms, 637 MB/s | 73 ms, 599 MB/s | +5.8 % | 14 % and 23 % |
| Write, LZ4 | 122 ms, 360 MB/s | 128 ms, 344 MB/s | +4.9 % | 16 % and 17 % |
| Read, uncompressed | 58 ms, 752 MB/s | 54 ms, 810 MB/s | −6.9 % | 33 % and 35 % |
| Read, LZ4 | 87 ms, 507 MB/s | 89 ms, 493 MB/s | +2.3 % | 13 % and 10 % |
| Peak resident size | 45–50 MiB | 46–50 MiB | within 1 % | |

The differences between the bindings are smaller than the differences between two runs of the same
binding, and their sign changes with the phase; at ten million agents they were −7 %, +5 %, +9 % and −2 %
in the same four phases. The peak resident size is the same to within a mebibyte, and still bounded by
the chunk: 46 to 52 MiB from one million agents to ten (the baseline is higher than the sandbox's 17 MiB
because HDF5 2.2 and the conda-forge runtime are larger, by the same amount for both). Writing to
memory rather than a disk, LZ4 still halves the write rate, 360 against 637 MB/s.

**What is decided and what is not.** Questions 1 and 2 are answered, and 4 except for Homebrew. On
question 3 the two machines agree: no systematic difference can be seen, and what can be seen is the
noise of the machine, which on the laptop is 10 to 35 % between runs. The rule asks for the dedicated
machine's 5 %, and a laptop with a boosting clock cannot resolve 5 %; what it can say is that a
systematic cost of that size would have to hide inside a spread three times larger and change sign from
phase to phase, which is not what a cost does. The reason is visible in the code: both bindings make the
same five `H5Dwrite` calls per block of 16,384 agents, and HighFive adds a few function calls around each,
nanoseconds against milliseconds. So the evidence supports the decision, and the two unlisted C calls
are the one fact the rule did not foresee; they are left for the decision to weigh rather than folded
into the rule after the event.

## References

1. Ntiniakou T, Osborne J, Zhao J, et al. *Open Benchmarking for Cell-Based Multiscale Models: Lessons from a
   Community Initiative.* bioRxiv 2025.07.16.664358 (2025). https://doi.org/10.1101/2025.07.16.664358
2. Breitwieser L, Hesam A, Yağlıkçı AG, Sadrosadati M, Rademakers F, Mutlu O. *TeraAgent: A Distributed
   Agent-Based Simulation Engine for Simulating Half a Trillion Agents.* arXiv:2509.24063 (2026); accepted
   for publication in IEEE Access. https://arxiv.org/abs/2509.24063
