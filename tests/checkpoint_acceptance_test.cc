// SPDX-License-Identifier: Apache-2.0
// The acceptance test of milestone 2 (#73): a small simulation saved through
// the store in two parts, restored with the strict resume, and continued,
// ends in exactly the state of the same simulation run without stopping; and
// a checkpoint interrupted before its commit leaves the previous one to
// restore from.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "abmio/checkpoint.h"
#include "abmio/records.h"
#include "abmio/store.h"

namespace acceptance {

struct Agent {
  uint64_t uid = 0;
};

struct Cell : Agent {
  double position[3] = {0, 0, 0};
  double velocity[3] = {0, 0, 0};
  double diameter = 1;
  int32_t divisions = 0;
  std::vector<uint64_t> neighbours;
  std::vector<double> history;  // grows by one value per step
};

}  // namespace acceptance

namespace abmio {

template <>
struct TypeTraits<acceptance::Agent> {
  static constexpr const char* kName = "acceptance::Agent";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("uid").Reference(), &acceptance::Agent::uid);
  }
};

template <>
struct TypeTraits<acceptance::Cell> {
  static constexpr const char* kName = "acceptance::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = acceptance::Agent;
  template <class V>
  static void Visit(V& v) {
    v(Field("position").Unit("um"), &acceptance::Cell::position);
    v(Field("velocity").Unit("um/min"), &acceptance::Cell::velocity);
    v(Field("diameter").Unit("um"), &acceptance::Cell::diameter);
    v(Field("divisions"), &acceptance::Cell::divisions);
    v(Field("neighbours").Reference(), &acceptance::Cell::neighbours);
    v(Field("history"), &acceptance::Cell::history);
  }
};

namespace {

using acceptance::Cell;

// Everything the simulation is: what a checkpoint must give back exactly.
struct State {
  SimulationRecord sim;
  std::vector<RandomStreamRecord> streams;  // one per half of the agents
  std::vector<Cell> cells;
};

// A counter-based generator: the value depends only on key and counter, so
// restoring the counter restores the stream.
double Uniform(RandomStreamRecord& s) {
  uint64_t z = s.key + 0x9e3779b97f4a7c15ULL * (++s.counter);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) * 0x1.0p-53;
}

State Start(uint64_t n) {
  State s;
  s.sim.time_unit = "min";
  s.streams = {{"half 0", 11, 0}, {"half 1", 23, 0}};
  s.cells.resize(n);
  for (uint64_t i = 0; i < n; ++i) {
    s.cells[i].uid = 100 + i;
    s.cells[i].position[0] = static_cast<double>(i % 10);
    s.cells[i].position[1] = static_cast<double>(i / 10);
    s.cells[i].diameter = 1 + 0.01 * static_cast<double>(i);
  }
  return s;
}

void Step(State& s) {
  const std::size_t n = s.cells.size();
  for (std::size_t i = 0; i < n; ++i) {
    Cell& c = s.cells[i];
    RandomStreamRecord& r = s.streams[i < n / 2 ? 0 : 1];
    for (int k = 0; k < 3; ++k) {
      c.velocity[k] = 0.9 * c.velocity[k] + (Uniform(r) - 0.5);
      c.position[k] += 0.1 * c.velocity[k];
    }
    if (Uniform(r) < 0.05) {
      ++c.divisions;
      c.diameter *= 0.8;
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    Cell& c = s.cells[i];
    c.neighbours.clear();
    for (std::size_t j = 0; j < n; ++j) {
      double d2 = 0;
      for (int k = 0; k < 3; ++k) {
        const double d = c.position[k] - s.cells[j].position[k];
        d2 += d * d;
      }
      if (j != i && d2 < 2.25) {
        c.neighbours.push_back(s.cells[j].uid);
      }
    }
    c.history.push_back(c.position[0]);
  }
  ++s.sim.step;
  s.sim.time += 0.5;
}

std::vector<ComponentRecord> Components() {
  ComponentRecord sim{"acceptance-sim", "1.0", "simulator", "", true, ""};
  ComponentRecord viz{"viewer", "3.1", "library", "", false, ""};
  return {sim, viz};
}

std::string Root() {
  std::string pattern = ::testing::TempDir() + "abmio_acceptance_XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  const char* made = ::mkdtemp(buffer.data());
  EXPECT_NE(made, nullptr);
  return made == nullptr ? pattern : std::string(made);
}

// Part p holds half p of the agents and stream p; part 0 also holds what
// belongs to the whole run.
Status WritePart(const std::string& directory, uint32_t p, State& s,
                 std::string* why) {
  const uint64_t n = s.cells.size();
  const uint64_t first = p == 0 ? 0 : n / 2;
  const uint64_t count = p == 0 ? n / 2 : n - n / 2;
  CheckpointWriter writer;
  CheckpointOptions options;
  options.chunk_rows = 7;  // several blocks per part
  Status st = writer.Open(directory + "/" + PartName(p), options, why);
  if (st == Status::kOk) {
    st = writer.WriteTable<Cell>(
        "agents/acceptance::Cell", count,
        [&s, first](uint64_t i) -> Cell& { return s.cells[first + i]; }, why);
  }
  if (st == Status::kOk) {
    st = writer.WriteRecord("records/abmio.RandomStream", s.streams[p], why);
  }
  if (st == Status::kOk && p == 0) {
    st = writer.WriteRecord(kSimulationRecord, s.sim, why);
    auto components = Components();
    if (st == Status::kOk) {
      st = writer.WriteTable<ComponentRecord>(
          kComponentsTable, components.size(),
          [&components](uint64_t i) -> ComponentRecord& {
            return components[i];
          },
          why);
    }
    std::vector<LineageRecord> none;
    if (st == Status::kOk) {
      st = writer.WriteTable<LineageRecord>(
          kLineageTable, 0,
          [&none](uint64_t i) -> LineageRecord& { return none[i]; }, why);
    }
  }
  if (st == Status::kOk) {
    st = writer.Close(why);
  }
  return st;
}

Status Save(const std::string& root, State& s, bool interrupted,
            std::string* why) {
  const std::string name = "step-" + std::to_string(s.sim.step);
  std::string directory;
  Status st = PrepareCheckpoint(root, name, &directory, why);
  for (uint32_t p = 0; st == Status::kOk && p < 2; ++p) {
    if (interrupted && p == 1) {
      return Status::kOk;  // killed after the first part: no commit
    }
    st = WritePart(directory, p, s, why);
  }
  return st == Status::kOk ? CommitCheckpoint(root, name, 2, why) : st;
}

Status Restore(const std::string& root, State* s, std::string* why) {
  FoundCheckpoint found;
  Status st = FindLatest(root, &found, why);
  if (st != Status::kOk) {
    return st;
  }
  if (!found.found || found.part_paths.size() != 2) {
    *why = "no complete checkpoint of two parts";
    return Status::kIo;
  }
  *s = State();
  s->streams.resize(2);
  for (uint32_t p = 0; p < 2 && st == Status::kOk; ++p) {
    CheckpointReader reader;
    CheckpointOptions options;
    options.chunk_rows = 5;  // other blocks than the writer's
    st = reader.Open(found.part_paths[p], options, why);
    if (st == Status::kOk && p == 0) {
      std::vector<LineageRecord> lineage;
      st = reader.CheckResume(Components(), {}, &lineage, why);
      if (st == Status::kOk) {
        st = reader.ReadRecord(kSimulationRecord, &s->sim, why);
      }
    }
    uint64_t rows = 0;
    if (st == Status::kOk) {
      st = reader.Rows<Cell>("agents/acceptance::Cell", &rows, why);
    }
    if (st == Status::kOk) {
      const std::size_t first = s->cells.size();
      s->cells.resize(first + rows);
      st = reader.ReadTable<Cell>(
          "agents/acceptance::Cell", rows,
          [s, first](uint64_t i) -> Cell& { return s->cells[first + i]; }, why);
    }
    if (st == Status::kOk) {
      st = reader.ReadRecord("records/abmio.RandomStream", &s->streams[p], why);
    }
  }
  return st;
}

void ExpectIdentical(const State& a, const State& b) {
  EXPECT_EQ(a.sim.step, b.sim.step);
  EXPECT_EQ(a.sim.time, b.sim.time);
  EXPECT_EQ(a.sim.time_unit, b.sim.time_unit);
  ASSERT_EQ(a.streams.size(), b.streams.size());
  for (std::size_t i = 0; i < a.streams.size(); ++i) {
    EXPECT_EQ(a.streams[i].owner, b.streams[i].owner);
    EXPECT_EQ(a.streams[i].key, b.streams[i].key);
    EXPECT_EQ(a.streams[i].counter, b.streams[i].counter);
  }
  ASSERT_EQ(a.cells.size(), b.cells.size());
  for (std::size_t i = 0; i < a.cells.size(); ++i) {
    const Cell& x = a.cells[i];
    const Cell& y = b.cells[i];
    EXPECT_EQ(x.uid, y.uid) << i;
    for (int k = 0; k < 3; ++k) {
      // Exact: bit for bit, not within a tolerance.
      EXPECT_EQ(x.position[k], y.position[k]) << i;
      EXPECT_EQ(x.velocity[k], y.velocity[k]) << i;
    }
    EXPECT_EQ(x.diameter, y.diameter) << i;
    EXPECT_EQ(x.divisions, y.divisions) << i;
    EXPECT_EQ(x.neighbours, y.neighbours) << i;
    EXPECT_EQ(x.history, y.history) << i;
  }
}

TEST(CheckpointAcceptance, ARestoredRunEndsExactlyAsAnUninterruptedOne) {
  const uint64_t n = 61;
  State straight = Start(n);
  for (int i = 0; i < 40; ++i) {
    Step(straight);
  }

  const std::string root = Root();
  std::string why;
  State first = Start(n);
  for (int i = 0; i < 25; ++i) {
    Step(first);
  }
  ASSERT_EQ(Save(root, first, false, &why), Status::kOk) << why;

  State restored;
  ASSERT_EQ(Restore(root, &restored, &why), Status::kOk) << why;
  ExpectIdentical(restored, first);
  for (int i = 0; i < 15; ++i) {
    Step(restored);
  }
  ExpectIdentical(restored, straight);
}

TEST(CheckpointAcceptance, AnInterruptedCheckpointLeavesThePreviousOne) {
  const std::string root = Root();
  std::string why;
  State s = Start(30);
  for (int i = 0; i < 10; ++i) {
    Step(s);
  }
  ASSERT_EQ(Save(root, s, false, &why), Status::kOk) << why;
  const State at_ten = s;
  for (int i = 0; i < 10; ++i) {
    Step(s);
  }
  ASSERT_EQ(Save(root, s, true, &why), Status::kOk) << why;

  State restored;
  ASSERT_EQ(Restore(root, &restored, &why), Status::kOk) << why;
  ExpectIdentical(restored, at_ten);
}

TEST(CheckpointAcceptance, AChangedSimulatorStopsTheRestore) {
  const std::string root = Root();
  std::string why;
  State s = Start(10);
  Step(s);
  ASSERT_EQ(Save(root, s, false, &why), Status::kOk) << why;
  FoundCheckpoint found;
  ASSERT_EQ(FindLatest(root, &found, &why), Status::kOk) << why;
  CheckpointReader reader;
  ASSERT_EQ(reader.Open(found.part_paths[0], {}, &why), Status::kOk);
  auto now = Components();
  now[0].version = "1.1";
  std::vector<LineageRecord> lineage;
  EXPECT_EQ(reader.CheckResume(now, {}, &lineage, &why), Status::kBadSchema);
  EXPECT_NE(why.find("acceptance-sim (simulator) was 1.0, is now 1.1"),
            std::string::npos)
      << why;
}

}  // namespace
}  // namespace abmio
