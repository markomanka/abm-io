// SPDX-License-Identifier: Apache-2.0
// The interface behind which the same small checkpoint writer and reader is
// written twice, once on HDF5's C API and once on HighFive, so that the tests
// and the harness do not know which they are running. The comparison and its
// decision rule are docs/RATIONALE.md entry 12; the results, docs/FINDINGS.md.
//
// What is written is the shape of a checkpoint (docs/INTEGRATION.md): a
// handful of agent columns in extendable, chunked, optionally LZ4-compressed
// datasets, written one chunk's worth at a time from a source that never holds
// more than that; a one-row record; two tables with strings. The agents are
// synthetic and deterministic in their index, so a reader checks them block by
// block without the writer's memory.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "abmio/records.h"

namespace abmio {
namespace h5binding {

/// One synthetic agent; Synthetic(i) is the agent with index i.
struct Agent {
  uint64_t uid = 0;
  double position[3] = {0, 0, 0};
  double diameter = 0;
  int32_t cell_type = 0;
  uint64_t mother = 0;
};

inline Agent Synthetic(uint64_t i) {
  Agent a;
  a.uid = i + 1;
  a.position[0] = static_cast<double>(i % 1000) * 0.5;
  a.position[1] = static_cast<double>((i / 1000) % 1000) * 0.5;
  a.position[2] = static_cast<double>(i / 1000000) * 0.5;
  a.diameter = 10.0 + static_cast<double>(i % 7) * 0.25;
  a.cell_type = static_cast<int32_t>(i % 3);
  a.mother = i == 0 ? 0 : (i - 1) / 2 + 1;
  return a;
}

/// One chunk's worth of agents, column by column, as the writer fills it.
struct Block {
  std::vector<uint64_t> uid;
  std::vector<double> position;  // 3 per agent
  std::vector<double> diameter;
  std::vector<int32_t> cell_type;
  std::vector<uint64_t> mother;
  std::size_t size() const { return uid.size(); }
  void Resize(std::size_t n) {
    uid.resize(n);
    position.resize(3 * n);
    diameter.resize(n);
    cell_type.resize(n);
    mother.resize(n);
  }
  void Fill(uint64_t first, std::size_t n) {
    Resize(n);
    for (std::size_t k = 0; k < n; ++k) {
      const Agent a = Synthetic(first + k);
      uid[k] = a.uid;
      position[3 * k] = a.position[0];
      position[3 * k + 1] = a.position[1];
      position[3 * k + 2] = a.position[2];
      diameter[k] = a.diameter;
      cell_type[k] = a.cell_type;
      mother[k] = a.mother;
    }
  }
  /// Agents of this block that differ from Synthetic(first + k).
  uint64_t Mismatches(uint64_t first) const {
    uint64_t bad = 0;
    for (std::size_t k = 0; k < size(); ++k) {
      const Agent a = Synthetic(first + k);
      if (uid[k] != a.uid || position[3 * k] != a.position[0] ||
          position[3 * k + 1] != a.position[1] ||
          position[3 * k + 2] != a.position[2] || diameter[k] != a.diameter ||
          cell_type[k] != a.cell_type || mother[k] != a.mother) {
        ++bad;
      }
    }
    return bad;
  }
};

/// The record and the two tables beside the agents.
struct Records {
  SimulationRecord simulation;
  std::vector<ComponentRecord> components;
  std::vector<RandomStreamRecord> streams;
};

/// The records every comparison writes, with strings of varying length.
Records SampleRecords();
bool operator==(const Records& a, const Records& b);

struct Layout {
  uint64_t agents = 0;
  uint64_t chunk = 4096;  // agents per chunk
  bool lz4 = true;
  std::size_t cache_bytes = 0;  // chunk cache per dataset; 0: HDF5's default
};

/// What a reader found: the agents, how many differed from Synthetic, and the
/// filters the agent datasets declared (so a test can check the LZ4 filter
/// went in).
struct ReadResult {
  uint64_t agents = 0;
  uint64_t mismatches = 0;
  Records records;
  std::vector<unsigned> filters;  // of /agents/cell/position
};

class Binding {
 public:
  virtual ~Binding() = default;
  virtual const char* Name() const = 0;
  /// Writes `layout.agents` synthetic agents and `records` to `path`, one
  /// block of `layout.chunk` agents at a time. Returns false and sets `why` on
  /// failure.
  virtual bool Write(const std::string& path, const Layout& layout,
                     const Records& records, std::string* why) = 0;
  /// Reads `path` back, `chunk` agents at a time, checking each block against
  /// Synthetic. Returns false and sets `why` on failure.
  virtual bool Read(const std::string& path, uint64_t chunk, ReadResult* out,
                    std::string* why) = 0;
};

std::unique_ptr<Binding> CApiBinding();
#ifdef ABMIO_HAVE_HIGHFIVE
std::unique_ptr<Binding> HighFiveBinding();
#endif

/// Every binding this build has.
std::vector<std::unique_ptr<Binding>> Bindings();

}  // namespace h5binding
}  // namespace abmio
