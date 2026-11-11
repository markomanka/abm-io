// SPDX-License-Identifier: Apache-2.0
// The container's storage layer: columns of every element type, written in
// blocks and read back whole and by range, strings, and the properties the
// contract asks of a file (docs/INTEGRATION.md, "Checkpoint files").
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "abmio/schema.h"
#include "h5store.h"

namespace abmio {
namespace h5 {
namespace {

std::string Path(const std::string& name) {
  return ::testing::TempDir() + "abmio_" + name + ".h5";
}

std::string Bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
}

// Rows × count values of T, written in blocks of uneven size so that blocks
// and chunks never line up, then read back whole and from the middle.
template <class T>
void RoundTrip(ElementType element, uint32_t count, bool lz4) {
  const uint64_t rows = 1000;
  std::vector<T> data(rows * count);
  for (uint64_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<T>(element == ElementType::kBool ? i % 2
                                                           : (i * 7 + 3) % 120);
  }
  const std::string path = Path("roundtrip");
  std::string why;
  {
    Id file;
    ASSERT_EQ(CreateFile(path, &file, &why), Status::kOk) << why;
    Id group;
    ASSERT_EQ(Group(file.get(), "agents/Cell", true, &group, &why), Status::kOk)
        << why;
    Column column;
    ColumnLayout layout;
    layout.chunk_rows = 64;
    layout.lz4 = lz4;
    ASSERT_EQ(Column::Create(group.get(), "field", element, count, layout,
                             &column, &why),
              Status::kOk)
        << why;
    uint64_t done = 0;
    for (uint64_t block : {1u, 63u, 64u, 65u, 300u, 507u}) {
      ASSERT_EQ(column.Append(data.data() + done * count, block, &why),
                Status::kOk)
          << why;
      done += block;
    }
    ASSERT_EQ(done, rows);
    EXPECT_EQ(column.rows(), rows);
  }
  Id file;
  ASSERT_EQ(OpenFile(path, &file, &why), Status::kOk) << why;
  Id group;
  ASSERT_EQ(Group(file.get(), "agents/Cell", false, &group, &why), Status::kOk)
      << why;
  Column column;
  ASSERT_EQ(Column::Open(group.get(), "field", &column, &why), Status::kOk)
      << why;
  EXPECT_EQ(column.rows(), rows);
  EXPECT_EQ(column.element(), element);
  EXPECT_EQ(column.count(), count);
  EXPECT_EQ(column.lz4(), lz4);

  std::vector<T> whole(rows * count);
  ASSERT_EQ(column.Read(0, rows, whole.data(), &why), Status::kOk) << why;
  EXPECT_EQ(whole, data);

  std::vector<T> middle(100 * count);
  ASSERT_EQ(column.Read(450, 100, middle.data(), &why), Status::kOk) << why;
  EXPECT_EQ(middle, std::vector<T>(data.begin() + 450 * count,
                                   data.begin() + 550 * count));
}

TEST(H5Store, EveryElementTypeSurvivesTheRoundTrip) {
  for (bool lz4 : {false, true}) {
    for (uint32_t count : {1u, 3u}) {
      RoundTrip<uint8_t>(ElementType::kBool, count, lz4);
      RoundTrip<char>(ElementType::kChar, count, lz4);
      RoundTrip<int8_t>(ElementType::kInt8, count, lz4);
      RoundTrip<uint8_t>(ElementType::kUInt8, count, lz4);
      RoundTrip<int16_t>(ElementType::kInt16, count, lz4);
      RoundTrip<uint16_t>(ElementType::kUInt16, count, lz4);
      RoundTrip<int32_t>(ElementType::kInt32, count, lz4);
      RoundTrip<uint32_t>(ElementType::kUInt32, count, lz4);
      RoundTrip<int64_t>(ElementType::kInt64, count, lz4);
      RoundTrip<uint64_t>(ElementType::kUInt64, count, lz4);
      RoundTrip<float>(ElementType::kFloat32, count, lz4);
      RoundTrip<double>(ElementType::kFloat64, count, lz4);
      RoundTrip<uint64_t>(ElementType::kAgentId, count, lz4);
    }
  }
}

TEST(H5Store, AnAgentIdentifierIsNotMistakenForAnInteger) {
  const std::string path = Path("agentid");
  std::string why;
  {
    Id file;
    ASSERT_EQ(CreateFile(path, &file, &why), Status::kOk) << why;
    Column a;
    Column b;
    ASSERT_EQ(Column::Create(file.get(), "mother", ElementType::kAgentId, 1, {},
                             &a, &why),
              Status::kOk);
    ASSERT_EQ(Column::Create(file.get(), "count", ElementType::kUInt64, 1, {},
                             &b, &why),
              Status::kOk);
  }
  Id file;
  ASSERT_EQ(OpenFile(path, &file, &why), Status::kOk) << why;
  Column a;
  Column b;
  ASSERT_EQ(Column::Open(file.get(), "mother", &a, &why), Status::kOk) << why;
  ASSERT_EQ(Column::Open(file.get(), "count", &b, &why), Status::kOk) << why;
  EXPECT_EQ(a.element(), ElementType::kAgentId);
  EXPECT_EQ(b.element(), ElementType::kUInt64);
}

TEST(H5Store, StringsSurviveTheRoundTrip) {
  const std::vector<std::string> values = {"biodynamo", "", "µm",
                                           "a longer one, with spaces",
                                           "sha256:0123456789abcdef"};
  const std::string path = Path("strings");
  std::string why;
  {
    Id file;
    ASSERT_EQ(CreateFile(path, &file, &why), Status::kOk) << why;
    StringColumn column;
    ASSERT_EQ(StringColumn::Create(file.get(), "name", 2, &column, &why),
              Status::kOk)
        << why;
    ASSERT_EQ(column.Append({values[0], values[1]}, &why), Status::kOk) << why;
    ASSERT_EQ(column.Append({values[2], values[3], values[4]}, &why),
              Status::kOk)
        << why;
  }
  Id file;
  ASSERT_EQ(OpenFile(path, &file, &why), Status::kOk) << why;
  StringColumn column;
  ASSERT_EQ(StringColumn::Open(file.get(), "name", &column, &why), Status::kOk)
      << why;
  EXPECT_EQ(column.rows(), values.size());
  std::vector<std::string> back;
  ASSERT_EQ(column.Read(0, values.size(), &back, &why), Status::kOk) << why;
  EXPECT_EQ(back, values);
  ASSERT_EQ(column.Read(2, 2, &back, &why), Status::kOk) << why;
  EXPECT_EQ(back, (std::vector<std::string>{values[2], values[3]}));
}

TEST(H5Store, AttributesSurviveTheRoundTrip) {
  const std::string path = Path("attributes");
  std::string why;
  {
    Id file;
    ASSERT_EQ(CreateFile(path, &file, &why), Status::kOk) << why;
    ASSERT_EQ(WriteAttribute(file.get(), "format", "abm-io checkpoint 1", &why),
              Status::kOk);
  }
  Id file;
  ASSERT_EQ(OpenFile(path, &file, &why), Status::kOk) << why;
  std::string value;
  ASSERT_EQ(ReadAttribute(file.get(), "format", &value, &why), Status::kOk)
      << why;
  EXPECT_EQ(value, "abm-io checkpoint 1");
  EXPECT_EQ(ReadAttribute(file.get(), "absent", &value, &why),
            Status::kCorrupt);
  EXPECT_NE(why.find("absent"), std::string::npos) << why;
}

TEST(H5Store, TheSameDataGivesTheSameBytes) {
  std::vector<double> data(1000);
  for (uint64_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<double>(i) * 0.5;
  }
  std::string why;
  for (const char* name : {"same_a", "same_b"}) {
    Id file;
    ASSERT_EQ(CreateFile(Path(name), &file, &why), Status::kOk) << why;
    Id group;
    ASSERT_EQ(Group(file.get(), "agents/Cell", true, &group, &why), Status::kOk)
        << why;
    Column column;
    ColumnLayout layout;
    layout.chunk_rows = 128;  // 1000 rows: the last chunk is partial
    ASSERT_EQ(Column::Create(group.get(), "diameter", ElementType::kFloat64, 1,
                             layout, &column, &why),
              Status::kOk)
        << why;
    ASSERT_EQ(column.Append(data.data(), data.size(), &why), Status::kOk)
        << why;
    StringColumn unit;
    ASSERT_EQ(StringColumn::Create(group.get(), "unit", 4, &unit, &why),
              Status::kOk)
        << why;
    ASSERT_EQ(unit.Append({"um"}, &why), Status::kOk) << why;
  }
  const std::string a = Bytes(Path("same_a"));
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a, Bytes(Path("same_b")));
}

TEST(H5Store, WhatIsNotThereIsReportedAndNotRead) {
  const std::string path = Path("errors");
  std::string why;
  Id missing;
  EXPECT_EQ(OpenFile(Path("no_such_file"), &missing, &why), Status::kIo);
  EXPECT_NE(why.find("missing"), std::string::npos) << why;

  std::vector<int32_t> data = {1, 2, 3, 4, 5};
  {
    Id file;
    ASSERT_EQ(CreateFile(path, &file, &why), Status::kOk) << why;
    Column column;
    ASSERT_EQ(Column::Create(file.get(), "x", ElementType::kInt32, 1, {},
                             &column, &why),
              Status::kOk);
    ASSERT_EQ(column.Append(data.data(), data.size(), &why), Status::kOk);
    Column bad;
    EXPECT_EQ(Column::Create(file.get(), "none", ElementType::kNone, 1, {},
                             &bad, &why),
              Status::kBadSchema);
    EXPECT_EQ(Column::Create(file.get(), "zero", ElementType::kInt32, 0, {},
                             &bad, &why),
              Status::kBadSchema);
  }
  Id file;
  ASSERT_EQ(OpenFile(path, &file, &why), Status::kOk) << why;
  Column column;
  EXPECT_EQ(Column::Open(file.get(), "y", &column, &why), Status::kCorrupt);
  EXPECT_NE(why.find("missing column y"), std::string::npos) << why;
  ASSERT_EQ(Column::Open(file.get(), "x", &column, &why), Status::kOk) << why;
  std::vector<int32_t> out(10);
  EXPECT_EQ(column.Read(3, 3, out.data(), &why), Status::kBadIndex);
  EXPECT_EQ(column.Read(6, 0, out.data(), &why), Status::kBadIndex);
  EXPECT_EQ(column.Read(5, 0, out.data(), &why),
            Status::kOk);  // nothing, at the end
  Id group;
  EXPECT_EQ(Group(file.get(), "agents", false, &group, &why), Status::kCorrupt);
}

}  // namespace
}  // namespace h5
}  // namespace abmio
