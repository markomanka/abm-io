// SPDX-License-Identifier: Apache-2.0
// The checkpoint writer: objects walked through the visitor protocol into
// tables, read back here through the storage layer to see what the file holds.
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "abmio/checkpoint.h"
#include "abmio/records.h"
#include "abmio/schema.h"
#include "container/h5store.h"

namespace writer_test {

struct Agent {
  uint64_t uid = 0;
};

struct Cell : Agent {
  double position[3] = {0, 0, 0};
  double diameter = 0;
  bool alive = true;
  int32_t kind = 0;
  std::string label;
  double cached = 0;  // never stored: it has no checkpoint role
};

struct WithVector {
  uint64_t uid = 0;
  std::vector<double> values;
  std::vector<uint64_t> neighbours;
  std::vector<bool> flags;
};

// A simulator's own container, described through FieldTraits.
struct SmallVector {
  int32_t data[4] = {0, 0, 0, 0};
  std::size_t size = 0;
};

struct WithOwnVector {
  SmallVector counts;
};

struct WithRecord {
  WithOwnVector inner;
};

struct Narrowed {
  double x = 0;
};

}  // namespace writer_test

namespace abmio {

template <>
struct FieldTraits<writer_test::SmallVector> {
  static constexpr Kind kKind = Kind::kVector;
  using Element = int32_t;
  static int32_t& At(writer_test::SmallVector& m, std::size_t i) {
    return m.data[i];
  }
  static std::size_t Size(const writer_test::SmallVector& m) { return m.size; }
};

template <>
struct TypeTraits<writer_test::Agent> {
  static constexpr const char* kName = "writer_test::Agent";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("uid").Reference(), &writer_test::Agent::uid);
  }
};

template <>
struct TypeTraits<writer_test::Cell> {
  static constexpr const char* kName = "writer_test::Cell";
  static constexpr uint32_t kVersion = 3;
  using Base = writer_test::Agent;
  template <class V>
  static void Visit(V& v) {
    v(Field("position").Unit("um"), &writer_test::Cell::position);
    v(Field("diameter").Unit("um"), &writer_test::Cell::diameter);
    v(Field("alive"), &writer_test::Cell::alive);
    v(Field("kind"), &writer_test::Cell::kind);
    v(Field("label"), &writer_test::Cell::label);
    v(Field("cached").Roles(Role::kExport), &writer_test::Cell::cached);
  }
};

template <>
struct TypeTraits<writer_test::WithVector> {
  static constexpr const char* kName = "writer_test::WithVector";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("uid").Reference(), &writer_test::WithVector::uid);
    v(Field("values"), &writer_test::WithVector::values);
    v(Field("neighbours").Reference(), &writer_test::WithVector::neighbours);
    v(Field("flags"), &writer_test::WithVector::flags);
  }
};

template <>
struct TypeTraits<writer_test::WithOwnVector> {
  static constexpr const char* kName = "writer_test::WithOwnVector";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("counts"), &writer_test::WithOwnVector::counts);
  }
};

template <>
struct TypeTraits<writer_test::WithRecord> {
  static constexpr const char* kName = "writer_test::WithRecord";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("inner"), &writer_test::WithRecord::inner);
  }
};

template <>
struct TypeTraits<writer_test::Narrowed> {
  static constexpr const char* kName = "writer_test::Narrowed";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("x").StoredAs(ElementType::kFloat32), &writer_test::Narrowed::x);
  }
};

namespace {

using writer_test::Cell;

std::string Path(const std::string& name) {
  return ::testing::TempDir() + "abmio_writer_" + name + ".h5";
}

std::string Bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
}

std::vector<Cell> Cells(uint64_t n) {
  std::vector<Cell> cells(n);
  for (uint64_t i = 0; i < n; ++i) {
    cells[i].uid = 1000 + i;
    cells[i].position[0] = static_cast<double>(i);
    cells[i].position[1] = static_cast<double>(i) * 0.5;
    cells[i].position[2] = -static_cast<double>(i);
    cells[i].diameter = 10.0 + static_cast<double>(i % 7);
    cells[i].alive = i % 3 != 0;
    cells[i].kind = static_cast<int32_t>(i % 5) - 2;
    cells[i].label = "cell " + std::to_string(i);
    cells[i].cached = 99;
  }
  return cells;
}

Status WriteCells(const std::string& path, std::vector<Cell>& cells,
                  uint64_t chunk_rows, std::string* why) {
  CheckpointWriter writer;
  CheckpointOptions options;
  options.chunk_rows = chunk_rows;
  Status s = writer.Open(path, options, why);
  if (s != Status::kOk) {
    return s;
  }
  s = writer.WriteTable<Cell>(
      "agents/writer_test::Cell", cells.size(),
      [&cells](uint64_t i) -> Cell& { return cells[i]; }, why);
  if (s != Status::kOk) {
    return s;
  }
  return writer.Close(why);
}

TEST(CheckpointWriter, AgentsBecomeColumnsBaseFieldsFirst) {
  std::vector<Cell> cells = Cells(1000);
  const std::string path = Path("cells");
  std::string why;
  // 128 rows to a chunk: eight blocks, the last one partial.
  ASSERT_EQ(WriteCells(path, cells, 128, &why), Status::kOk) << why;

  h5::Id file;
  ASSERT_EQ(h5::OpenFile(path, &file, &why), Status::kOk) << why;
  std::string format;
  ASSERT_EQ(h5::ReadAttribute(file.get(), "format", &format, &why), Status::kOk)
      << why;
  EXPECT_EQ(format, "abm-io checkpoint");
  h5::Id group;
  ASSERT_EQ(
      h5::Group(file.get(), "agents/writer_test::Cell", false, &group, &why),
      Status::kOk)
      << why;
  std::string type;
  ASSERT_EQ(h5::ReadAttribute(group.get(), "abmio_type", &type, &why),
            Status::kOk)
      << why;
  EXPECT_EQ(type, "writer_test::Cell");

  h5::Column uid;
  ASSERT_EQ(h5::Column::Open(group.get(), "uid", &uid, &why), Status::kOk)
      << why;
  EXPECT_EQ(uid.element(),
            ElementType::kAgentId);  // the base type's field, a reference
  ASSERT_EQ(uid.rows(), 1000u);
  std::vector<uint64_t> uids(1000);
  ASSERT_EQ(uid.Read(0, 1000, uids.data(), &why), Status::kOk) << why;
  EXPECT_EQ(uids[0], 1000u);
  EXPECT_EQ(uids[999], 1999u);

  h5::Column position;
  ASSERT_EQ(h5::Column::Open(group.get(), "position", &position, &why),
            Status::kOk)
      << why;
  EXPECT_EQ(position.count(), 3u);
  std::vector<double> xyz(3000);
  ASSERT_EQ(position.Read(0, 1000, xyz.data(), &why), Status::kOk) << why;
  EXPECT_EQ(xyz[3 * 640 + 0], 640.0);
  EXPECT_EQ(xyz[3 * 640 + 1], 320.0);
  EXPECT_EQ(xyz[3 * 640 + 2], -640.0);

  h5::Column alive;
  ASSERT_EQ(h5::Column::Open(group.get(), "alive", &alive, &why), Status::kOk)
      << why;
  EXPECT_EQ(alive.element(), ElementType::kBool);
  std::vector<uint8_t> flags(1000);
  ASSERT_EQ(alive.Read(0, 1000, flags.data(), &why), Status::kOk) << why;
  EXPECT_EQ(flags[0], 0);
  EXPECT_EQ(flags[1], 1);
  EXPECT_EQ(flags[999], 0);

  h5::Column kind;
  ASSERT_EQ(h5::Column::Open(group.get(), "kind", &kind, &why), Status::kOk)
      << why;
  std::vector<int32_t> kinds(1000);
  ASSERT_EQ(kind.Read(0, 1000, kinds.data(), &why), Status::kOk) << why;
  EXPECT_EQ(kinds[0], -2);
  EXPECT_EQ(kinds[4], 2);

  h5::StringColumn label;
  ASSERT_EQ(h5::StringColumn::Open(group.get(), "label", &label, &why),
            Status::kOk)
      << why;
  std::vector<std::string> labels;
  ASSERT_EQ(label.Read(127, 3, &labels, &why), Status::kOk)
      << why;  // across a block boundary
  EXPECT_EQ(labels,
            (std::vector<std::string>{"cell 127", "cell 128", "cell 129"}));

  // A field without the checkpoint role is not in the file.
  h5::Column cached;
  EXPECT_EQ(h5::Column::Open(group.get(), "cached", &cached, &why),
            Status::kCorrupt);
}

TEST(CheckpointWriter, AnEmptyTableStillHasItsColumns) {
  std::vector<Cell> none;
  const std::string path = Path("empty");
  std::string why;
  ASSERT_EQ(WriteCells(path, none, 64, &why), Status::kOk) << why;
  h5::Id file;
  ASSERT_EQ(h5::OpenFile(path, &file, &why), Status::kOk) << why;
  h5::Id group;
  ASSERT_EQ(
      h5::Group(file.get(), "agents/writer_test::Cell", false, &group, &why),
      Status::kOk);
  h5::Column diameter;
  ASSERT_EQ(h5::Column::Open(group.get(), "diameter", &diameter, &why),
            Status::kOk)
      << why;
  EXPECT_EQ(diameter.rows(), 0u);
  h5::StringColumn label;
  ASSERT_EQ(h5::StringColumn::Open(group.get(), "label", &label, &why),
            Status::kOk)
      << why;
  EXPECT_EQ(label.rows(), 0u);
}

TEST(CheckpointWriter, RecordsAreTablesWithOneRow) {
  const std::string path = Path("records");
  std::string why;
  CheckpointWriter writer;
  ASSERT_EQ(writer.Open(path, {}, &why), Status::kOk) << why;
  SimulationRecord sim;
  sim.step = 4200;
  sim.time = 420.5;
  sim.time_unit = "min";
  ASSERT_EQ(writer.WriteRecord("records/abmio.Simulation", sim, &why),
            Status::kOk)
      << why;

  std::vector<ComponentRecord> components(3);
  components[0].name = "biodynamo";
  components[0].version = "1.06";
  components[0].role = "simulator";
  components[1].name = "paraview";
  components[1].version = "6.2";
  components[1].affects_results = false;
  components[2].name = "weights";
  components[2].digest = "sha256:abc";
  ASSERT_EQ(writer.WriteTable<ComponentRecord>(
                "components", components.size(),
                [&components](uint64_t i) -> ComponentRecord& {
                  return components[i];
                },
                &why),
            Status::kOk)
      << why;
  ASSERT_EQ(writer.Close(&why), Status::kOk) << why;

  h5::Id file;
  ASSERT_EQ(h5::OpenFile(path, &file, &why), Status::kOk) << why;
  h5::Id group;
  ASSERT_EQ(
      h5::Group(file.get(), "records/abmio.Simulation", false, &group, &why),
      Status::kOk);
  h5::Column step;
  ASSERT_EQ(h5::Column::Open(group.get(), "step", &step, &why), Status::kOk)
      << why;
  ASSERT_EQ(step.rows(), 1u);
  uint64_t value = 0;
  ASSERT_EQ(step.Read(0, 1, &value, &why), Status::kOk) << why;
  EXPECT_EQ(value, 4200u);
  h5::StringColumn unit;
  ASSERT_EQ(h5::StringColumn::Open(group.get(), "time_unit", &unit, &why),
            Status::kOk)
      << why;
  std::vector<std::string> units;
  ASSERT_EQ(unit.Read(0, 1, &units, &why), Status::kOk) << why;
  EXPECT_EQ(units, std::vector<std::string>{"min"});

  h5::Id table;
  ASSERT_EQ(h5::Group(file.get(), "components", false, &table, &why),
            Status::kOk)
      << why;
  h5::StringColumn name;
  ASSERT_EQ(h5::StringColumn::Open(table.get(), "name", &name, &why),
            Status::kOk)
      << why;
  std::vector<std::string> names;
  ASSERT_EQ(name.Read(0, 3, &names, &why), Status::kOk) << why;
  EXPECT_EQ(names,
            (std::vector<std::string>{"biodynamo", "paraview", "weights"}));
  h5::Column affects;
  ASSERT_EQ(h5::Column::Open(table.get(), "affects_results", &affects, &why),
            Status::kOk)
      << why;
  std::vector<uint8_t> flags(3);
  ASSERT_EQ(affects.Read(0, 3, flags.data(), &why), Status::kOk) << why;
  EXPECT_EQ(flags, (std::vector<uint8_t>{1, 0, 1}));
}

TEST(CheckpointWriter, TheFileCarriesTheSchemaOfWhatItHolds) {
  std::vector<Cell> cells = Cells(10);
  const std::string path = Path("schema");
  std::string why;
  ASSERT_EQ(WriteCells(path, cells, 64, &why), Status::kOk) << why;
  h5::Id file;
  ASSERT_EQ(h5::OpenFile(path, &file, &why), Status::kOk) << why;
  h5::StringColumn schema;
  ASSERT_EQ(h5::StringColumn::Open(file.get(), "schema", &schema, &why),
            Status::kOk)
      << why;
  std::vector<std::string> text;
  ASSERT_EQ(schema.Read(0, 1, &text, &why), Status::kOk) << why;
  const std::string& json = text[0];
  // Both the type and its base, with the version, the unit and the role.
  EXPECT_NE(json.find("\"name\":\"writer_test::Agent\""), std::string::npos)
      << json;
  EXPECT_NE(json.find("\"name\":\"writer_test::Cell\""), std::string::npos);
  EXPECT_NE(json.find("\"version\":3"), std::string::npos);
  EXPECT_NE(json.find("\"name\":\"position\",\"kind\":\"array\""),
            std::string::npos)
      << json;
  EXPECT_NE(json.find("\"unit\":\"um\""), std::string::npos);
  EXPECT_NE(json.find("\"name\":\"cached\""),
            std::string::npos);  // described, though not stored
  EXPECT_NE(json.find("\"roles\":[\"export\"]"), std::string::npos) << json;
}

TEST(CheckpointWriter, NestedRecordsAndNarrowedFieldsAreWritten) {
  std::string why;
  CheckpointWriter writer;
  ASSERT_EQ(writer.Open(Path("refused"), {}, &why), Status::kOk) << why;

  // A nested record's fields become columns in a group named after it.
  std::vector<writer_test::WithRecord> records(2);
  EXPECT_EQ(writer.WriteTable<writer_test::WithRecord>(
                "a", 2,
                [&records](uint64_t i) -> writer_test::WithRecord& {
                  return records[i];
                },
                &why),
            Status::kOk)
      << why;

  // StoredAs does not narrow a checkpoint: the column keeps the member's type.
  writer_test::Narrowed narrowed;
  EXPECT_EQ(writer.WriteRecord("b", narrowed, &why), Status::kOk) << why;

  // The same table twice.
  SimulationRecord sim;
  ASSERT_EQ(writer.WriteRecord("records/abmio.Simulation", sim, &why),
            Status::kOk)
      << why;
  EXPECT_EQ(writer.WriteRecord("records/abmio.Simulation", sim, &why),
            Status::kBadSchema);
  EXPECT_NE(why.find("already written"), std::string::npos) << why;
  ASSERT_EQ(writer.Close(&why), Status::kOk) << why;

  // Nothing can be written before Open or after Close.
  EXPECT_EQ(writer.WriteRecord("late", sim, &why), Status::kIo);
  EXPECT_EQ(writer.Close(&why), Status::kIo);
  CheckpointWriter unopened;
  EXPECT_EQ(unopened.WriteRecord("early", sim, &why), Status::kIo);
}

TEST(CheckpointWriter, TheSameSimulationGivesTheSameBytes) {
  std::string why;
  for (const char* name : {"same_a", "same_b"}) {
    std::vector<Cell> cells = Cells(777);
    ASSERT_EQ(WriteCells(Path(name), cells, 100, &why), Status::kOk) << why;
  }
  const std::string a = Bytes(Path("same_a"));
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a, Bytes(Path("same_b")));
}

std::vector<uint64_t> ReadAll(const h5::Column& c) {
  std::vector<uint64_t> out(c.rows());
  std::string why;
  EXPECT_EQ(c.Read(0, c.rows(), out.data(), &why), Status::kOk) << why;
  return out;
}

// Object i has i % 4 values, so blocks of three cross chunk boundaries with
// empty arrays at the start, in the middle and at the end of a block.
std::vector<writer_test::WithVector> Vectors(uint64_t n) {
  std::vector<writer_test::WithVector> objects(n);
  for (uint64_t i = 0; i < n; ++i) {
    objects[i].uid = i;
    for (uint64_t k = 0; k < i % 4; ++k) {
      objects[i].values.push_back(static_cast<double>(10 * i + k));
      objects[i].neighbours.push_back(100 + i + k);
      objects[i].flags.push_back((i + k) % 2 == 0);
    }
  }
  return objects;
}

Status WriteVectors(const std::string& path,
                    std::vector<writer_test::WithVector>& objects,
                    uint64_t chunk_rows, std::string* why) {
  CheckpointWriter writer;
  CheckpointOptions options;
  options.chunk_rows = chunk_rows;
  Status s = writer.Open(path, options, why);
  if (s == Status::kOk) {
    s = writer.WriteTable<writer_test::WithVector>(
        "agents/writer_test::WithVector", objects.size(),
        [&objects](uint64_t i) -> writer_test::WithVector& {
          return objects[i];
        },
        why);
  }
  if (s == Status::kOk) {
    s = writer.Close(why);
  }
  return s;
}

TEST(CheckpointWriter, VariableSizeArraysBecomeValuesAndOffsets) {
  const uint64_t n = 11;
  auto objects = Vectors(n);
  const std::string path = Path("vectors");
  std::string why;
  ASSERT_EQ(WriteVectors(path, objects, 3, &why), Status::kOk) << why;

  h5::Id file;
  ASSERT_EQ(h5::OpenFile(path, &file, &why), Status::kOk) << why;
  h5::Id group;
  ASSERT_EQ(h5::Group(file.get(), "agents/writer_test::WithVector/values",
                      false, &group, &why),
            Status::kOk)
      << why;
  h5::Column values, offsets;
  ASSERT_EQ(h5::Column::Open(group.get(), "values", &values, &why), Status::kOk)
      << why;
  ASSERT_EQ(h5::Column::Open(group.get(), "offsets", &offsets, &why),
            Status::kOk)
      << why;
  EXPECT_EQ(values.element(), ElementType::kFloat64);
  EXPECT_EQ(offsets.element(), ElementType::kUInt64);
  ASSERT_EQ(offsets.rows(), n + 1);

  const std::vector<uint64_t> ends = ReadAll(offsets);
  std::vector<double> all(values.rows());
  ASSERT_EQ(values.Read(0, values.rows(), all.data(), &why), Status::kOk)
      << why;
  EXPECT_EQ(ends.front(), 0u);
  EXPECT_EQ(ends.back(), values.rows());
  for (uint64_t i = 0; i < n; ++i) {
    std::vector<double> got(
        all.begin() + static_cast<std::ptrdiff_t>(ends[i]),
        all.begin() + static_cast<std::ptrdiff_t>(ends[i + 1]));
    EXPECT_EQ(got, objects[i].values) << "object " << i;
  }

  // Agent identifiers and booleans take the same layout.
  h5::Id neighbours;
  ASSERT_EQ(h5::Group(file.get(), "agents/writer_test::WithVector/neighbours",
                      false, &neighbours, &why),
            Status::kOk);
  h5::Column ids, id_offsets;
  ASSERT_EQ(h5::Column::Open(neighbours.get(), "values", &ids, &why),
            Status::kOk);
  ASSERT_EQ(h5::Column::Open(neighbours.get(), "offsets", &id_offsets, &why),
            Status::kOk);
  EXPECT_EQ(ReadAll(id_offsets), ends);
  const std::vector<uint64_t> id_values = ReadAll(ids);
  for (uint64_t i = 0; i < n; ++i) {
    for (uint64_t k = ends[i]; k < ends[i + 1]; ++k) {
      EXPECT_EQ(id_values[k], objects[i].neighbours[k - ends[i]]);
    }
  }
  h5::Id flags;
  ASSERT_EQ(h5::Group(file.get(), "agents/writer_test::WithVector/flags", false,
                      &flags, &why),
            Status::kOk);
  h5::Column flag_values;
  ASSERT_EQ(h5::Column::Open(flags.get(), "values", &flag_values, &why),
            Status::kOk);
  EXPECT_EQ(flag_values.element(), ElementType::kBool);
  EXPECT_EQ(flag_values.rows(), values.rows());
}

TEST(CheckpointWriter, TheChunkSizeDoesNotChangeWhatVectorsHold) {
  auto objects = Vectors(23);
  std::string why;
  for (uint64_t chunk : {1u, 4u, 64u}) {
    const std::string path = Path("vectors_chunk" + std::to_string(chunk));
    ASSERT_EQ(WriteVectors(path, objects, chunk, &why), Status::kOk) << why;
    h5::Id file;
    ASSERT_EQ(h5::OpenFile(path, &file, &why), Status::kOk) << why;
    h5::Id group;
    ASSERT_EQ(h5::Group(file.get(), "agents/writer_test::WithVector/neighbours",
                        false, &group, &why),
              Status::kOk);
    h5::Column values, offsets;
    ASSERT_EQ(h5::Column::Open(group.get(), "values", &values, &why),
              Status::kOk);
    ASSERT_EQ(h5::Column::Open(group.get(), "offsets", &offsets, &why),
              Status::kOk);
    std::vector<uint64_t> expected_values, expected_offsets = {0};
    for (const auto& o : objects) {
      expected_values.insert(expected_values.end(), o.neighbours.begin(),
                             o.neighbours.end());
      expected_offsets.push_back(expected_values.size());
    }
    EXPECT_EQ(ReadAll(values), expected_values) << "chunk " << chunk;
    EXPECT_EQ(ReadAll(offsets), expected_offsets) << "chunk " << chunk;
  }
}

TEST(CheckpointWriter, AnEmptyTableHasOneOffset) {
  std::vector<writer_test::WithVector> none;
  const std::string path = Path("vectors_empty");
  std::string why;
  ASSERT_EQ(WriteVectors(path, none, 8, &why), Status::kOk) << why;
  h5::Id file;
  ASSERT_EQ(h5::OpenFile(path, &file, &why), Status::kOk) << why;
  h5::Id group;
  ASSERT_EQ(h5::Group(file.get(), "agents/writer_test::WithVector/values",
                      false, &group, &why),
            Status::kOk);
  h5::Column values, offsets;
  ASSERT_EQ(h5::Column::Open(group.get(), "values", &values, &why),
            Status::kOk);
  ASSERT_EQ(h5::Column::Open(group.get(), "offsets", &offsets, &why),
            Status::kOk);
  EXPECT_EQ(values.rows(), 0u);
  EXPECT_EQ(ReadAll(offsets), std::vector<uint64_t>{0});
}

TEST(CheckpointWriter, ASimulatorsOwnVectorGoesThroughFieldTraits) {
  std::vector<writer_test::WithOwnVector> objects(3);
  objects[0].counts.size = 2;
  objects[0].counts.data[0] = 7;
  objects[0].counts.data[1] = -1;
  objects[2].counts.size = 1;
  objects[2].counts.data[0] = 42;
  const std::string path = Path("vectors_traits");
  std::string why;
  CheckpointWriter writer;
  ASSERT_EQ(writer.Open(path, {}, &why), Status::kOk) << why;
  ASSERT_EQ(writer.WriteTable<writer_test::WithOwnVector>(
                "agents/own", objects.size(),
                [&objects](uint64_t i) -> writer_test::WithOwnVector& {
                  return objects[i];
                },
                &why),
            Status::kOk)
      << why;
  ASSERT_EQ(writer.Close(&why), Status::kOk) << why;
  h5::Id file;
  ASSERT_EQ(h5::OpenFile(path, &file, &why), Status::kOk) << why;
  h5::Id group;
  ASSERT_EQ(h5::Group(file.get(), "agents/own/counts", false, &group, &why),
            Status::kOk);
  h5::Column values, offsets;
  ASSERT_EQ(h5::Column::Open(group.get(), "values", &values, &why),
            Status::kOk);
  ASSERT_EQ(h5::Column::Open(group.get(), "offsets", &offsets, &why),
            Status::kOk);
  std::vector<int32_t> got(values.rows());
  ASSERT_EQ(values.Read(0, values.rows(), got.data(), &why), Status::kOk);
  EXPECT_EQ(got, (std::vector<int32_t>{7, -1, 42}));
  EXPECT_EQ(ReadAll(offsets), (std::vector<uint64_t>{0, 2, 2, 3}));
}

}  // namespace
}  // namespace abmio
