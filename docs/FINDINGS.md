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

## References

1. Ntiniakou T, Osborne J, Zhao J, et al. *Open Benchmarking for Cell-Based Multiscale Models: Lessons from a
   Community Initiative.* bioRxiv 2025.07.16.664358 (2025). https://doi.org/10.1101/2025.07.16.664358
2. Breitwieser L, Hesam A, Yağlıkçı AG, Sadrosadati M, Rademakers F, Mutlu O. *TeraAgent: A Distributed
   Agent-Based Simulation Engine for Simulating Half a Trillion Agents.* arXiv:2509.24063 (2026); accepted
   for publication in IEEE Access. https://arxiv.org/abs/2509.24063
