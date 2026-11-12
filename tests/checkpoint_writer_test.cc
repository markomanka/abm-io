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
  std::vector<double> values;
};

struct Narrowed {
  double x = 0;
};

}  // namespace writer_test

namespace abmio {

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
    v(Field("values"), &writer_test::WithVector::values);
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

TEST(CheckpointWriter, WhatIsNotWrittenYetIsRefusedByName) {
  std::string why;
  CheckpointWriter writer;
  ASSERT_EQ(writer.Open(Path("refused"), {}, &why), Status::kOk) << why;

  std::vector<writer_test::WithVector> vectors(2);
  EXPECT_EQ(writer.WriteTable<writer_test::WithVector>(
                "a", 2,
                [&vectors](uint64_t i) -> writer_test::WithVector& {
                  return vectors[i];
                },
                &why),
            Status::kBadSchema);
  EXPECT_NE(why.find("writer_test::WithVector.values"), std::string::npos)
      << why;
  EXPECT_NE(why.find("do not write yet"), std::string::npos) << why;

  writer_test::Narrowed narrowed;
  EXPECT_EQ(writer.WriteRecord("b", narrowed, &why), Status::kBadSchema);
  EXPECT_NE(why.find("StoredAs"), std::string::npos) << why;

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

}  // namespace
}  // namespace abmio
