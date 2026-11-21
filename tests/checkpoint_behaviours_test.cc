// SPDX-License-Identifier: Apache-2.0
// Nested records and lists of behaviours in checkpoints (#72): what is
// written comes back exactly, the layout is what INTEGRATION.md says, and a
// model or a file that does not match is refused with what differs.
#include <gtest/gtest.h>
#include <hdf5.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "abmio/checkpoint.h"
#include "abmio/schema.h"

namespace beh_test {

struct Clock {
  double period = 0;
  std::vector<double> marks;
};

struct Phase {
  int32_t index = 0;
  std::string name;
  Clock clock;  // a record inside a record
};

struct Behaviour {
  virtual ~Behaviour() = default;
  bool copy_to_new = true;
};

struct Grow : Behaviour {
  double rate = 0;
};

struct Secrete : Behaviour {
  std::string substance;
  std::vector<double> amounts;
  Phase phase;  // a record inside a behaviour
};

struct Unlisted : Behaviour {};

struct Cell {
  uint64_t uid = 0;
  Phase phase;
  std::vector<std::unique_ptr<Behaviour>> behaviours;
  double diameter = 0;
};

// The same cell, as a model whose behaviour base lists only Grow.
struct GrowOnly {
  virtual ~GrowOnly() = default;
  bool copy_to_new = true;
};
struct Grow2 : GrowOnly {
  double rate = 0;
};
struct CellGrowOnly {
  uint64_t uid = 0;
  Phase phase;
  std::vector<std::unique_ptr<GrowOnly>> behaviours;
  double diameter = 0;
};

// The same cell, with a field more in the nested record.
struct Phase2 {
  int32_t index = 0;
  std::string name;
  Clock clock;
  double extra = 0;
};
struct CellPhase2 {
  uint64_t uid = 0;
  Phase2 phase;
  std::vector<std::unique_ptr<Behaviour>> behaviours;
  double diameter = 0;
};

}  // namespace beh_test

namespace abmio {

#define BEH_TYPE(T, NAME, BASE, BODY)          \
  template <>                                  \
  struct TypeTraits<beh_test::T> {             \
    static constexpr const char* kName = NAME; \
    static constexpr uint32_t kVersion = 1;    \
    using Base = BASE;                         \
    template <class V>                         \
    static void Visit(V& v) {                  \
      BODY                                     \
    }                                          \
  };

BEH_TYPE(Clock, "beh_test::Clock", void,
         v(Field("period").Unit("min"), &beh_test::Clock::period);
         v(Field("marks"), &beh_test::Clock::marks);)
BEH_TYPE(Phase, "beh_test::Phase", void,
         v(Field("index"), &beh_test::Phase::index);
         v(Field("name"), &beh_test::Phase::name);
         v(Field("clock"), &beh_test::Phase::clock);)
BEH_TYPE(Phase2, "beh_test::Phase", void,
         v(Field("index"), &beh_test::Phase2::index);
         v(Field("name"), &beh_test::Phase2::name);
         v(Field("clock"), &beh_test::Phase2::clock);
         v(Field("extra"), &beh_test::Phase2::extra);)

template <>
struct TypeTraits<beh_test::Behaviour> {
  static constexpr const char* kName = "beh_test::Behaviour";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  using Subtypes = std::tuple<beh_test::Grow, beh_test::Secrete>;
  template <class V>
  static void Visit(V& v) {
    v(Field("copy_to_new"), &beh_test::Behaviour::copy_to_new);
  }
};
BEH_TYPE(Grow, "beh_test::Grow", beh_test::Behaviour,
         v(Field("rate"), &beh_test::Grow::rate);)
BEH_TYPE(Secrete, "beh_test::Secrete", beh_test::Behaviour,
         v(Field("substance"), &beh_test::Secrete::substance);
         v(Field("amounts"), &beh_test::Secrete::amounts);
         v(Field("phase"), &beh_test::Secrete::phase);)

template <>
struct TypeTraits<beh_test::GrowOnly> {
  static constexpr const char* kName = "beh_test::Behaviour";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  using Subtypes = std::tuple<beh_test::Grow2>;
  template <class V>
  static void Visit(V& v) {
    v(Field("copy_to_new"), &beh_test::GrowOnly::copy_to_new);
  }
};
BEH_TYPE(Grow2, "beh_test::Grow", beh_test::GrowOnly,
         v(Field("rate"), &beh_test::Grow2::rate);)

#define BEH_CELL(T, PHASE)                          \
  v(Field("uid").Reference(), &beh_test::T::uid);   \
  v(Field("phase"), &beh_test::T::phase);           \
  v(Field("behaviours"), &beh_test::T::behaviours); \
  v(Field("diameter").Unit("um"), &beh_test::T::diameter);
BEH_TYPE(Cell, "beh_test::Cell", void, BEH_CELL(Cell, Phase))
BEH_TYPE(CellGrowOnly, "beh_test::Cell", void, BEH_CELL(CellGrowOnly, Phase))
BEH_TYPE(CellPhase2, "beh_test::Cell", void, BEH_CELL(CellPhase2, Phase2))

#undef BEH_CELL
#undef BEH_TYPE

namespace {

using beh_test::Cell;
const char* const kTable = "agents/beh_test::Cell";

std::string Path(const std::string& name) {
  return ::testing::TempDir() + "abmio_behaviours_" + name + ".h5";
}

// Cell i has i % 4 behaviours, alternating Grow and Secrete, so blocks cross
// list boundaries with empty lists at the start, middle and end.
std::vector<Cell> Cells(uint64_t n) {
  std::vector<Cell> cells(n);
  for (uint64_t i = 0; i < n; ++i) {
    Cell& c = cells[i];
    c.uid = 500 + i;
    c.diameter = 1.5 * static_cast<double>(i);
    c.phase.index = static_cast<int32_t>(i % 3);
    c.phase.name = "G" + std::to_string(i % 2);
    c.phase.clock.period = 0.5 + static_cast<double>(i);
    for (uint64_t k = 0; k < i % 3; ++k) {
      c.phase.clock.marks.push_back(static_cast<double>(10 * i + k));
    }
    for (uint64_t b = 0; b < i % 4; ++b) {
      if ((i + b) % 2 == 0) {
        auto g = std::make_unique<beh_test::Grow>();
        g->rate = 0.1 * static_cast<double>(i + b);
        g->copy_to_new = b % 2 == 0;
        c.behaviours.push_back(std::move(g));
      } else {
        auto s = std::make_unique<beh_test::Secrete>();
        s->substance = "S" + std::to_string(b);
        for (uint64_t a = 0; a < b; ++a) {
          s->amounts.push_back(static_cast<double>(i * 100 + a));
        }
        s->phase.index = static_cast<int32_t>(b);
        s->phase.name = "p" + std::to_string(i);
        s->phase.clock.period = static_cast<double>(b);
        s->phase.clock.marks = {static_cast<double>(i)};
        c.behaviours.push_back(std::move(s));
      }
    }
  }
  return cells;
}

Status Write(const std::string& path, std::vector<Cell>& cells, uint64_t chunk,
             std::string* why) {
  CheckpointWriter writer;
  CheckpointOptions options;
  options.chunk_rows = chunk;
  Status s = writer.Open(path, options, why);
  if (s == Status::kOk) {
    s = writer.WriteTable<Cell>(
        kTable, cells.size(),
        [&cells](uint64_t i) -> Cell& { return cells[i]; }, why);
  }
  return s == Status::kOk ? writer.Close(why) : s;
}

template <class T>
Status Read(const std::string& path, uint64_t chunk, std::vector<T>* out,
            std::string* why) {
  CheckpointReader reader;
  CheckpointOptions options;
  options.chunk_rows = chunk;
  Status s = reader.Open(path, options, why);
  uint64_t rows = 0;
  if (s == Status::kOk) {
    s = reader.Rows<T>(kTable, &rows, why);
  }
  if (s == Status::kOk) {
    out->clear();
    out->resize(rows);
    s = reader.ReadTable<T>(
        kTable, rows, [out](uint64_t i) -> T& { return (*out)[i]; }, why);
  }
  return s;
}

void ExpectPhase(const beh_test::Phase& a, const beh_test::Phase& b) {
  EXPECT_EQ(a.index, b.index);
  EXPECT_EQ(a.name, b.name);
  EXPECT_EQ(a.clock.period, b.clock.period);
  EXPECT_EQ(a.clock.marks, b.clock.marks);
}

void ExpectSame(const Cell& a, const Cell& b, uint64_t i) {
  SCOPED_TRACE("cell " + std::to_string(i));
  EXPECT_EQ(a.uid, b.uid);
  EXPECT_EQ(a.diameter, b.diameter);
  ExpectPhase(a.phase, b.phase);
  ASSERT_EQ(a.behaviours.size(), b.behaviours.size());
  for (std::size_t k = 0; k < a.behaviours.size(); ++k) {
    EXPECT_EQ(a.behaviours[k]->copy_to_new, b.behaviours[k]->copy_to_new);
    auto* ga = dynamic_cast<beh_test::Grow*>(a.behaviours[k].get());
    auto* gb = dynamic_cast<beh_test::Grow*>(b.behaviours[k].get());
    auto* sa = dynamic_cast<beh_test::Secrete*>(a.behaviours[k].get());
    auto* sb = dynamic_cast<beh_test::Secrete*>(b.behaviours[k].get());
    ASSERT_EQ(ga == nullptr, gb == nullptr) << "behaviour " << k;
    ASSERT_EQ(sa == nullptr, sb == nullptr) << "behaviour " << k;
    if (ga != nullptr) {
      EXPECT_EQ(ga->rate, gb->rate);
    } else {
      EXPECT_EQ(sa->substance, sb->substance);
      EXPECT_EQ(sa->amounts, sb->amounts);
      ExpectPhase(sa->phase, sb->phase);
    }
  }
}

TEST(CheckpointBehaviours, NestedRecordsAndBehavioursComeBackExactly) {
  auto cells = Cells(17);
  std::string why;
  for (uint64_t write_chunk : {1u, 3u, 64u}) {
    const std::string path = Path("roundtrip" + std::to_string(write_chunk));
    ASSERT_EQ(Write(path, cells, write_chunk, &why), Status::kOk) << why;
    for (uint64_t read_chunk : {1u, 2u, 64u}) {
      std::vector<Cell> back;
      ASSERT_EQ(Read(path, read_chunk, &back, &why), Status::kOk) << why;
      ASSERT_EQ(back.size(), cells.size());
      for (uint64_t i = 0; i < cells.size(); ++i) {
        ExpectSame(back[i], cells[i], i);
      }
    }
  }
}

TEST(CheckpointBehaviours, AnUnlistedBehaviourIsRefusedWhenWriting) {
  auto cells = Cells(4);
  cells[2].behaviours.push_back(std::make_unique<beh_test::Unlisted>());
  std::string why;
  EXPECT_EQ(Write(Path("unlisted"), cells, 8, &why), Status::kBadSchema);
  EXPECT_NE(why.find("behaviours"), std::string::npos) << why;
  EXPECT_NE(why.find("not listed"), std::string::npos) << why;
}

TEST(CheckpointBehaviours, AListIsReadIntoAnEmptyOne) {
  auto cells = Cells(4);
  const std::string path = Path("nonempty");
  std::string why;
  ASSERT_EQ(Write(path, cells, 8, &why), Status::kOk) << why;
  CheckpointReader reader;
  ASSERT_EQ(reader.Open(path, {}, &why), Status::kOk) << why;
  std::vector<Cell> back(4);
  back[1].behaviours.push_back(std::make_unique<beh_test::Grow>());
  EXPECT_EQ(
      reader.ReadTable<Cell>(
          kTable, 4, [&back](uint64_t i) -> Cell& { return back[i]; }, &why),
      Status::kBadSchema);
  EXPECT_NE(why.find("already holds elements"), std::string::npos) << why;
}

TEST(CheckpointBehaviours, AModelThatDiffersIsRefusedByName) {
  auto cells = Cells(6);
  const std::string path = Path("models");
  std::string why;
  ASSERT_EQ(Write(path, cells, 8, &why), Status::kOk) << why;

  // A model whose behaviours do not include Secrete.
  std::vector<beh_test::CellGrowOnly> grow_only;
  EXPECT_EQ(Read(path, 8, &grow_only, &why), Status::kBadSchema);
  EXPECT_NE(why.find("behaviours/beh_test::Secrete"), std::string::npos) << why;

  // A model whose nested record has a field more.
  std::vector<beh_test::CellPhase2> phase2;
  EXPECT_EQ(Read(path, 8, &phase2, &why), Status::kBadSchema);
  EXPECT_NE(why.find("phase/extra"), std::string::npos) << why;
}

// Overwrites one element of the list's type identifiers.
void SetType(const std::string& path, hsize_t element, uint64_t id) {
  hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
  ASSERT_GE(file, 0);
  const std::string name = std::string(kTable) + "/behaviours/types";
  hid_t set = H5Dopen2(file, name.c_str(), H5P_DEFAULT);
  ASSERT_GE(set, 0);
  hid_t space = H5Dget_space(set);
  hsize_t one = 1;
  H5Sselect_hyperslab(space, H5S_SELECT_SET, &element, nullptr, &one, nullptr);
  hid_t memory = H5Screate_simple(1, &one, nullptr);
  EXPECT_GE(H5Dwrite(set, H5T_NATIVE_UINT64, memory, space, H5P_DEFAULT, &id),
            0);
  H5Sclose(memory);
  H5Sclose(space);
  H5Dclose(set);
  H5Fclose(file);
}

TEST(CheckpointBehaviours, DamagedTypeIdentifiersAreRefused) {
  std::string why;
  {
    auto cells = Cells(6);
    const std::string path = Path("unknown_id");
    ASSERT_EQ(Write(path, cells, 8, &why), Status::kOk) << why;
    SetType(path, 0, 12345);
    std::vector<Cell> back;
    EXPECT_EQ(Read(path, 8, &back, &why), Status::kBadSchema);
    EXPECT_NE(why.find("does not list"), std::string::npos) << why;
  }
  {
    // Cell 1 holds one Secrete (element 0); calling it a Grow leaves the
    // Grow table short and a Secrete row unused.
    auto cells = Cells(6);
    const std::string path = Path("swapped_id");
    ASSERT_EQ(Write(path, cells, 8, &why), Status::kOk) << why;
    SetType(path, 0, detail::IdOf<beh_test::Grow>());
    std::vector<Cell> back;
    EXPECT_EQ(Read(path, 8, &back, &why), Status::kCorrupt);
    EXPECT_NE(why.find("rows"), std::string::npos) << why;
  }
}

}  // namespace
}  // namespace abmio
