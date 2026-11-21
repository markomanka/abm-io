// SPDX-License-Identifier: Apache-2.0
// The checkpoint reader: what the writer wrote comes back field for field, a
// table that does not match the model is refused with the difference named,
// and damaged offsets are refused before they are used.
#include <gtest/gtest.h>
#include <hdf5.h>

#include <cstdint>
#include <string>
#include <vector>

#include "abmio/checkpoint.h"
#include "abmio/records.h"
#include "abmio/schema.h"

namespace reader_test {

struct Agent {
  uint64_t uid = 0;
};

// A simulator's own container, described through FieldTraits.
struct SmallVector {
  int32_t data[4] = {0, 0, 0, 0};
  std::size_t size = 0;
  bool operator==(const SmallVector& o) const {
    for (std::size_t i = 0; i < size; ++i) {
      if (data[i] != o.data[i]) {
        return false;
      }
    }
    return size == o.size;
  }
};

struct Cell : Agent {
  double position[3] = {0, 0, 0};
  bool alive = true;
  int32_t kind = 0;
  std::string label;
  std::vector<double> concentrations;
  std::vector<uint64_t> neighbours;
  std::vector<bool> flags;
  SmallVector counts;
  double cached = 0;  // never stored: it has no checkpoint role
};

// Types that claim Cell's name but differ from it, as a changed model would.
struct CellWithVolume : Agent {
  double position[3] = {0, 0, 0};
  bool alive = true;
  int32_t kind = 0;
  std::string label;
  std::vector<double> concentrations;
  std::vector<uint64_t> neighbours;
  std::vector<bool> flags;
  SmallVector counts;
  double volume = 0;
};

struct CellWithoutLabel : Agent {
  double position[3] = {0, 0, 0};
  bool alive = true;
  int32_t kind = 0;
  std::vector<double> concentrations;
  std::vector<uint64_t> neighbours;
  std::vector<bool> flags;
  SmallVector counts;
};

struct CellWithWideKind : Agent {
  double position[3] = {0, 0, 0};
  bool alive = true;
  int64_t kind = 0;
  std::string label;
  std::vector<double> concentrations;
  std::vector<uint64_t> neighbours;
  std::vector<bool> flags;
  SmallVector counts;
};

// The same fields under a new version of the type, and in other units.
struct CellVersion2 : Cell {};
struct CellInMillimetres : Cell {};

struct Other {
  double x = 0;
};

struct Narrowed {
  double x = 0;
};

}  // namespace reader_test

namespace abmio {

template <>
struct FieldTraits<reader_test::SmallVector> {
  static constexpr Kind kKind = Kind::kVector;
  using Element = int32_t;
  static int32_t& At(reader_test::SmallVector& m, std::size_t i) {
    return m.data[i];
  }
  static std::size_t Size(const reader_test::SmallVector& m) { return m.size; }
  static void Resize(reader_test::SmallVector& m, std::size_t n) {
    m.size = n < 4 ? n : 4;
  }
};

template <>
struct TypeTraits<reader_test::Agent> {
  static constexpr const char* kName = "reader_test::Agent";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("uid").Reference(), &reader_test::Agent::uid);
  }
};

// The fields every variant of Cell shares, in Cell's order.
#define READER_TEST_CELL_FIELDS(T)               \
  v(Field("position").Unit("um"), &T::position); \
  v(Field("alive"), &T::alive);                  \
  v(Field("kind"), &T::kind);

#define READER_TEST_CELL_VECTORS(T)                   \
  v(Field("concentrations"), &T::concentrations);     \
  v(Field("neighbours").Reference(), &T::neighbours); \
  v(Field("flags"), &T::flags);                       \
  v(Field("counts"), &T::counts);

template <>
struct TypeTraits<reader_test::Cell> {
  static constexpr const char* kName = "reader_test::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = reader_test::Agent;
  template <class V>
  static void Visit(V& v) {
    READER_TEST_CELL_FIELDS(reader_test::Cell)
    v(Field("label"), &reader_test::Cell::label);
    READER_TEST_CELL_VECTORS(reader_test::Cell)
    v(Field("cached").Roles(Role::kExport), &reader_test::Cell::cached);
  }
};

template <>
struct TypeTraits<reader_test::CellWithVolume> {
  static constexpr const char* kName = "reader_test::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = reader_test::Agent;
  template <class V>
  static void Visit(V& v) {
    READER_TEST_CELL_FIELDS(reader_test::CellWithVolume)
    v(Field("label"), &reader_test::CellWithVolume::label);
    READER_TEST_CELL_VECTORS(reader_test::CellWithVolume)
    v(Field("volume"), &reader_test::CellWithVolume::volume);
  }
};

template <>
struct TypeTraits<reader_test::CellWithoutLabel> {
  static constexpr const char* kName = "reader_test::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = reader_test::Agent;
  template <class V>
  static void Visit(V& v) {
    READER_TEST_CELL_FIELDS(reader_test::CellWithoutLabel)
    READER_TEST_CELL_VECTORS(reader_test::CellWithoutLabel)
  }
};

template <>
struct TypeTraits<reader_test::CellWithWideKind> {
  static constexpr const char* kName = "reader_test::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = reader_test::Agent;
  template <class V>
  static void Visit(V& v) {
    READER_TEST_CELL_FIELDS(reader_test::CellWithWideKind)
    v(Field("label"), &reader_test::CellWithWideKind::label);
    READER_TEST_CELL_VECTORS(reader_test::CellWithWideKind)
  }
};

template <>
struct TypeTraits<reader_test::CellVersion2> {
  static constexpr const char* kName = "reader_test::Cell";
  static constexpr uint32_t kVersion = 2;
  using Base = reader_test::Agent;
  template <class V>
  static void Visit(V& v) {
    TypeTraits<reader_test::Cell>::Visit(v);
  }
};

template <>
struct TypeTraits<reader_test::CellInMillimetres> {
  static constexpr const char* kName = "reader_test::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = reader_test::Agent;
  template <class V>
  static void Visit(V& v) {
    v(Field("position").Unit("mm"), &reader_test::Cell::position);
    v(Field("alive"), &reader_test::Cell::alive);
    v(Field("kind"), &reader_test::Cell::kind);
    v(Field("label"), &reader_test::Cell::label);
    READER_TEST_CELL_VECTORS(reader_test::Cell)
    v(Field("cached").Roles(Role::kExport), &reader_test::Cell::cached);
  }
};

template <>
struct TypeTraits<reader_test::Narrowed> {
  static constexpr const char* kName = "reader_test::Narrowed";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("x").StoredAs(ElementType::kFloat32), &reader_test::Narrowed::x);
  }
};

template <>
struct TypeTraits<reader_test::Other> {
  static constexpr const char* kName = "reader_test::Other";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("x"), &reader_test::Other::x);
  }
};

#undef READER_TEST_CELL_FIELDS
#undef READER_TEST_CELL_VECTORS

namespace {

using reader_test::Cell;

const char* const kTable = "agents/reader_test::Cell";

std::string Path(const std::string& name) {
  return ::testing::TempDir() + "abmio_reader_" + name + ".h5";
}

// Object i has i % 4 elements in each vector, so blocks cross chunk
// boundaries with empty arrays at the start, middle and end.
std::vector<Cell> Cells(uint64_t n) {
  std::vector<Cell> cells(n);
  for (uint64_t i = 0; i < n; ++i) {
    Cell& c = cells[i];
    c.uid = 1000 + i;
    c.position[0] = static_cast<double>(i);
    c.position[1] = static_cast<double>(i) * 0.5;
    c.position[2] = -static_cast<double>(i);
    c.alive = i % 3 != 0;
    c.kind = static_cast<int32_t>(i % 5) - 2;
    c.label = i % 6 == 0 ? "" : "cell " + std::to_string(i);
    for (uint64_t k = 0; k < i % 4; ++k) {
      c.concentrations.push_back(0.25 * static_cast<double>(i + k));
      c.neighbours.push_back(2000 + i + k);
      c.flags.push_back((i + k) % 2 == 0);
      c.counts.data[k] = static_cast<int32_t>(i * 10 + k) - 7;
    }
    c.counts.size = i % 4;
    c.cached = 99;
  }
  return cells;
}

Status Write(const std::string& path, std::vector<Cell>& cells,
             uint64_t chunk_rows, std::string* why) {
  CheckpointWriter writer;
  CheckpointOptions options;
  options.chunk_rows = chunk_rows;
  Status s = writer.Open(path, options, why);
  if (s == Status::kOk) {
    s = writer.WriteTable<Cell>(
        kTable, cells.size(),
        [&cells](uint64_t i) -> Cell& { return cells[i]; }, why);
  }
  SimulationRecord sim;
  sim.step = 1234;
  sim.time = 61.5;
  sim.time_unit = "min";
  if (s == Status::kOk) {
    s = writer.WriteRecord("records/abmio.Simulation", sim, why);
  }
  if (s == Status::kOk) {
    s = writer.Close(why);
  }
  return s;
}

template <class T>
Status ReadAs(const std::string& path, uint64_t chunk_rows, std::vector<T>* out,
              std::string* why) {
  CheckpointReader reader;
  CheckpointOptions options;
  options.chunk_rows = chunk_rows;
  Status s = reader.Open(path, options, why);
  uint64_t rows = 0;
  if (s == Status::kOk) {
    s = reader.Rows<T>(kTable, &rows, why);
  }
  if (s == Status::kOk) {
    out->assign(rows, T());
    s = reader.ReadTable<T>(
        kTable, rows, [out](uint64_t i) -> T& { return (*out)[i]; }, why);
  }
  return s;
}

void ExpectSame(const Cell& a, const Cell& b, uint64_t i) {
  EXPECT_EQ(a.uid, b.uid) << i;
  for (int k = 0; k < 3; ++k) {
    EXPECT_EQ(a.position[k], b.position[k]) << i;
  }
  EXPECT_EQ(a.alive, b.alive) << i;
  EXPECT_EQ(a.kind, b.kind) << i;
  EXPECT_EQ(a.label, b.label) << i;
  EXPECT_EQ(a.concentrations, b.concentrations) << i;
  EXPECT_EQ(a.neighbours, b.neighbours) << i;
  EXPECT_EQ(a.flags, b.flags) << i;
  EXPECT_TRUE(a.counts == b.counts) << i;
}

TEST(CheckpointReader, WhatWasWrittenComesBackFieldForField) {
  auto cells = Cells(23);
  std::string why;
  for (uint64_t write_chunk : {1u, 4u, 64u}) {
    const std::string path = Path("roundtrip" + std::to_string(write_chunk));
    ASSERT_EQ(Write(path, cells, write_chunk, &why), Status::kOk) << why;
    for (uint64_t read_chunk : {1u, 3u, 64u}) {
      std::vector<Cell> back;
      ASSERT_EQ(ReadAs(path, read_chunk, &back, &why), Status::kOk) << why;
      ASSERT_EQ(back.size(), cells.size());
      for (uint64_t i = 0; i < cells.size(); ++i) {
        ExpectSame(back[i], cells[i], i);
        EXPECT_EQ(back[i].cached, 0) << "a field without the role is kept";
      }
    }
  }
}

TEST(CheckpointReader, RecordsComeBack) {
  auto cells = Cells(2);
  const std::string path = Path("record");
  std::string why;
  ASSERT_EQ(Write(path, cells, 8, &why), Status::kOk) << why;
  CheckpointReader reader;
  ASSERT_EQ(reader.Open(path, {}, &why), Status::kOk) << why;
  SimulationRecord sim;
  ASSERT_EQ(reader.ReadRecord("records/abmio.Simulation", &sim, &why),
            Status::kOk)
      << why;
  EXPECT_EQ(sim.step, 1234u);
  EXPECT_EQ(sim.time, 61.5);
  EXPECT_EQ(sim.time_unit, "min");
}

TEST(CheckpointReader, AnEmptyTableReadsAsNoObjects) {
  std::vector<Cell> none;
  const std::string path = Path("empty");
  std::string why;
  ASSERT_EQ(Write(path, none, 8, &why), Status::kOk) << why;
  std::vector<Cell> back(3);
  ASSERT_EQ(ReadAs(path, 8, &back, &why), Status::kOk) << why;
  EXPECT_TRUE(back.empty());
}

TEST(CheckpointReader, ATableThatDiffersFromTheModelIsRefusedByName) {
  auto cells = Cells(5);
  const std::string path = Path("mismatch");
  std::string why;
  ASSERT_EQ(Write(path, cells, 8, &why), Status::kOk) << why;

  std::vector<reader_test::CellWithVolume> with_volume;
  EXPECT_EQ(ReadAs(path, 8, &with_volume, &why), Status::kBadSchema);
  EXPECT_NE(why.find("volume"), std::string::npos) << why;
  EXPECT_NE(why.find("not in the file"), std::string::npos) << why;

  std::vector<reader_test::CellWithoutLabel> without_label;
  EXPECT_EQ(ReadAs(path, 8, &without_label, &why), Status::kBadSchema);
  EXPECT_NE(why.find("label"), std::string::npos) << why;
  EXPECT_NE(why.find("model does not"), std::string::npos) << why;

  std::vector<reader_test::CellWithWideKind> wide;
  EXPECT_EQ(ReadAs(path, 8, &wide, &why), Status::kBadSchema);
  EXPECT_NE(why.find("kind"), std::string::npos) << why;
  EXPECT_NE(why.find("int32"), std::string::npos) << why;

  std::vector<reader_test::CellVersion2> version2;
  EXPECT_EQ(ReadAs(path, 8, &version2, &why), Status::kBadSchema);
  EXPECT_NE(why.find("reader_test::Cell has version 1 in the file and 2 in "
                     "the model"),
            std::string::npos)
      << why;

  std::vector<reader_test::CellInMillimetres> millimetres;
  EXPECT_EQ(ReadAs(path, 8, &millimetres, &why), Status::kBadSchema);
  EXPECT_NE(why.find("position has unit um in the file and mm in the model"),
            std::string::npos)
      << why;

  CheckpointReader reader;
  ASSERT_EQ(reader.Open(path, {}, &why), Status::kOk) << why;
  reader_test::Other other;
  EXPECT_EQ(reader.ReadRecord(kTable, &other, &why), Status::kBadSchema);
  EXPECT_NE(why.find("holds reader_test::Cell"), std::string::npos) << why;

  std::vector<Cell> back(4);
  EXPECT_EQ(
      reader.ReadTable<Cell>(
          kTable, 4, [&back](uint64_t i) -> Cell& { return back[i]; }, &why),
      Status::kSizeMismatch);
  EXPECT_NE(why.find("holds 5 objects"), std::string::npos) << why;
}

// Overwrites the offsets of one vector field, as a damaged file would have.
void SetOffsets(const std::string& path, const std::string& field,
                const std::vector<uint64_t>& offsets) {
  hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
  ASSERT_GE(file, 0);
  const std::string name = std::string(kTable) + "/" + field + "/offsets";
  hid_t set = H5Dopen2(file, name.c_str(), H5P_DEFAULT);
  ASSERT_GE(set, 0);
  EXPECT_GE(H5Dwrite(set, H5T_NATIVE_UINT64, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     offsets.data()),
            0);
  H5Dclose(set);
  H5Fclose(file);
}

TEST(CheckpointReader, DamagedOffsetsAreRefusedBeforeUse) {
  // Five cells hold 0, 1, 2, 3, 0 concentrations: offsets 0 0 1 3 6 6.
  struct Case {
    const char* what;
    std::vector<uint64_t> offsets;
    const char* message;
  };
  const Case cases[] = {
      {"not from 0", {1, 1, 2, 3, 6, 6}, "start at 0"},
      {"short of the end", {0, 0, 1, 3, 5, 5}, "end at the number"},
      {"decreasing", {0, 2, 1, 3, 6, 6}, "decrease"},
      {"past the end", {0, 0, 9, 3, 6, 6}, "pass the end"},
  };
  for (const Case& c : cases) {
    auto cells = Cells(5);
    const std::string path = Path("damaged");
    std::string why;
    ASSERT_EQ(Write(path, cells, 2, &why), Status::kOk) << why;
    SetOffsets(path, "concentrations", c.offsets);
    std::vector<Cell> back;
    EXPECT_EQ(ReadAs(path, 2, &back, &why), Status::kCorrupt) << c.what;
    EXPECT_NE(why.find("concentrations"), std::string::npos) << why;
    EXPECT_NE(why.find(c.message), std::string::npos) << c.what << ": " << why;
  }
}

TEST(CheckpointReader, NothingIsReadBeforeOpen) {
  CheckpointReader reader;
  SimulationRecord sim;
  std::string why;
  EXPECT_EQ(reader.ReadRecord("records/abmio.Simulation", &sim, &why),
            Status::kIo);
  EXPECT_EQ(reader.Open(Path("absent_file"), {}, &why), Status::kIo);
}

TEST(CheckpointReader, StoredAsDoesNotNarrowACheckpoint) {
  const std::string path = Path("narrowed");
  reader_test::Narrowed written;
  written.x = 0.1;  // not representable in 32 bits
  std::string why;
  {
    CheckpointWriter writer;
    ASSERT_EQ(writer.Open(path, {}, &why), Status::kOk) << why;
    ASSERT_EQ(writer.WriteRecord("records/n", written, &why), Status::kOk)
        << why;
    ASSERT_EQ(writer.Close(&why), Status::kOk) << why;
  }
  CheckpointReader reader;
  ASSERT_EQ(reader.Open(path, {}, &why), Status::kOk) << why;
  reader_test::Narrowed back;
  ASSERT_EQ(reader.ReadRecord("records/n", &back, &why), Status::kOk) << why;
  EXPECT_EQ(back.x, 0.1) << "bit for bit, not rounded to float";
}

// Rewrites a string attribute on the file's root, as another writer would.
void SetRootAttribute(const std::string& path, const char* name,
                      const char* value) {
  hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
  ASSERT_GE(file, 0);
  if (H5Aexists(file, name) > 0) {
    H5Adelete(file, name);
  }
  hid_t type = H5Tcopy(H5T_C_S1);
  H5Tset_size(type, H5T_VARIABLE);
  hid_t space = H5Screate(H5S_SCALAR);
  hid_t attr = H5Acreate2(file, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
  ASSERT_GE(attr, 0);
  EXPECT_GE(H5Awrite(attr, type, &value), 0);
  H5Aclose(attr);
  H5Sclose(space);
  H5Tclose(type);
  H5Fclose(file);
}

TEST(CheckpointReader, AnUnknownFormatVersionIsRefused) {
  auto cells = Cells(3);
  const std::string path = Path("format");
  std::string why;
  ASSERT_EQ(Write(path, cells, 8, &why), Status::kOk) << why;
  SetRootAttribute(path, "format_version", "2");
  CheckpointReader reader;
  EXPECT_EQ(reader.Open(path, {}, &why), Status::kBadHeader);
  EXPECT_NE(why.find("format version 2"), std::string::npos) << why;
  SimulationRecord sim;
  EXPECT_EQ(reader.ReadRecord("records/abmio.Simulation", &sim, &why),
            Status::kIo)
      << "a refused file stays closed";
}

TEST(CheckpointReader, AFileThatIsNotACheckpointIsRefused) {
  const std::string path = Path("plain");
  hid_t file = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  ASSERT_GE(file, 0);
  H5Fclose(file);
  CheckpointReader reader;
  std::string why;
  EXPECT_EQ(reader.Open(path, {}, &why), Status::kBadHeader);
  EXPECT_NE(why.find("not an abm-io checkpoint"), std::string::npos) << why;

  auto cells = Cells(2);
  const std::string other = Path("other_format");
  ASSERT_EQ(Write(other, cells, 8, &why), Status::kOk) << why;
  SetRootAttribute(other, "format", "something else");
  EXPECT_EQ(reader.Open(other, {}, &why), Status::kBadHeader);
}

}  // namespace
}  // namespace abmio
