// SPDX-License-Identifier: Apache-2.0
// The store of checkpoints: a part appears only when closed, a checkpoint
// only when committed, and a reader that follows the pointer finds the newest
// complete checkpoint or refuses with what is wrong.
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "abmio/checkpoint.h"
#include "abmio/records.h"
#include "abmio/store.h"

namespace abmio {
namespace {

bool Exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

void WriteText(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
}

// A fresh directory for each test.
std::string Root(const std::string& name) {
  std::string pattern =
      ::testing::TempDir() + "abmio_store_" + name + "_XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  const char* made = ::mkdtemp(buffer.data());
  EXPECT_NE(made, nullptr);
  return made == nullptr ? pattern : std::string(made);
}

// Writes part `part` of a checkpoint whose Simulation record holds `step`.
Status WritePart(const std::string& directory, uint32_t part, uint64_t step,
                 std::string* why) {
  CheckpointWriter writer;
  Status s = writer.Open(directory + "/" + PartName(part), {}, why);
  SimulationRecord sim;
  sim.step = step;
  sim.time_unit = "min";
  if (s == Status::kOk) {
    s = writer.WriteRecord("records/abmio.Simulation", sim, why);
  }
  if (s == Status::kOk) {
    s = writer.Close(why);
  }
  return s;
}

TEST(CheckpointStore, PartNames) {
  EXPECT_EQ(PartName(0), "part-00000.h5");
  EXPECT_EQ(PartName(123456), "part-123456.h5");
}

TEST(CheckpointStore, APartAppearsUnderItsNameOnlyWhenClosed) {
  const std::string root = Root("part");
  std::string directory, why;
  ASSERT_EQ(PrepareCheckpoint(root, "step-1", &directory, &why), Status::kOk)
      << why;
  const std::string path = directory + "/" + PartName(0);
  CheckpointWriter writer;
  ASSERT_EQ(writer.Open(path, {}, &why), Status::kOk) << why;
  SimulationRecord sim;
  ASSERT_EQ(writer.WriteRecord("records/abmio.Simulation", sim, &why),
            Status::kOk);
  EXPECT_FALSE(Exists(path));
  EXPECT_TRUE(Exists(path + ".tmp"));
  ASSERT_EQ(writer.Close(&why), Status::kOk) << why;
  EXPECT_TRUE(Exists(path));
  EXPECT_FALSE(Exists(path + ".tmp"));
}

TEST(CheckpointStore, ACommittedCheckpointIsFoundWithItsParts) {
  const std::string root = Root("commit");
  std::string why;
  FoundCheckpoint found;
  ASSERT_EQ(FindLatest(root, &found, &why), Status::kOk) << why;
  EXPECT_FALSE(found.found) << "nothing written yet";

  for (uint64_t step : {10u, 20u}) {
    const std::string name = "step-" + std::to_string(step);
    std::string directory;
    ASSERT_EQ(PrepareCheckpoint(root, name, &directory, &why), Status::kOk)
        << why;
    for (uint32_t part = 0; part < 3; ++part) {
      ASSERT_EQ(WritePart(directory, part, step, &why), Status::kOk) << why;
    }
    ASSERT_EQ(CommitCheckpoint(root, name, 3, &why), Status::kOk) << why;
    ASSERT_EQ(FindLatest(root, &found, &why), Status::kOk) << why;
    ASSERT_TRUE(found.found);
    EXPECT_EQ(found.manifest.checkpoint, name);
    ASSERT_EQ(found.part_paths.size(), 3u);
    for (const std::string& path : found.part_paths) {
      CheckpointReader reader;
      ASSERT_EQ(reader.Open(path, {}, &why), Status::kOk) << why;
      SimulationRecord sim;
      ASSERT_EQ(reader.ReadRecord("records/abmio.Simulation", &sim, &why),
                Status::kOk)
          << why;
      EXPECT_EQ(sim.step, step);
    }
  }
  std::string again;
  EXPECT_EQ(PrepareCheckpoint(root, "step-10", &again, &why),
            Status::kBadSchema);
  EXPECT_NE(why.find("already complete"), std::string::npos) << why;
}

TEST(CheckpointStore, AnUnfinishedCheckpointLeavesThePreviousOneVisible) {
  const std::string root = Root("unfinished");
  std::string directory, why;
  ASSERT_EQ(PrepareCheckpoint(root, "step-1", &directory, &why), Status::kOk);
  ASSERT_EQ(WritePart(directory, 0, 1, &why), Status::kOk) << why;
  ASSERT_EQ(CommitCheckpoint(root, "step-1", 1, &why), Status::kOk) << why;

  // The next checkpoint stops after one of its two parts.
  ASSERT_EQ(PrepareCheckpoint(root, "step-2", &directory, &why), Status::kOk);
  ASSERT_EQ(WritePart(directory, 0, 2, &why), Status::kOk) << why;
  EXPECT_EQ(CommitCheckpoint(root, "step-2", 2, &why), Status::kIo);
  EXPECT_NE(why.find("part-00001.h5"), std::string::npos) << why;

  FoundCheckpoint found;
  ASSERT_EQ(FindLatest(root, &found, &why), Status::kOk) << why;
  ASSERT_TRUE(found.found);
  EXPECT_EQ(found.manifest.checkpoint, "step-1");
}

TEST(CheckpointStore, ADamagedStoreIsRefusedWithWhatIsWrong) {
  const std::string root = Root("damaged");
  std::string directory, why;
  ASSERT_EQ(PrepareCheckpoint(root, "step-1", &directory, &why), Status::kOk);
  ASSERT_EQ(WritePart(directory, 0, 1, &why), Status::kOk) << why;
  ASSERT_EQ(CommitCheckpoint(root, "step-1", 1, &why), Status::kOk) << why;
  const std::string part = directory + "/" + PartName(0);
  FoundCheckpoint found;

  // A part whose size no longer matches the manifest.
  {
    std::ofstream f(part, std::ios::binary | std::ios::app);
    f << "x";
  }
  EXPECT_EQ(FindLatest(root, &found, &why), Status::kCorrupt);
  EXPECT_NE(why.find("size differs"), std::string::npos) << why;
  EXPECT_FALSE(found.found);

  // A missing part.
  ASSERT_EQ(std::remove(part.c_str()), 0);
  EXPECT_EQ(FindLatest(root, &found, &why), Status::kCorrupt);
  EXPECT_NE(why.find("missing"), std::string::npos) << why;

  // A pointer to a checkpoint with no manifest.
  WriteText(root + "/latest", PointerJson("step-9"));
  EXPECT_EQ(FindLatest(root, &found, &why), Status::kCorrupt);
  EXPECT_NE(why.find("no manifest"), std::string::npos) << why;

  // A pointer that tries to leave the directory.
  WriteText(root + "/latest",
            "{\"abmio_pointer\": 1, \"checkpoint\": \"../etc\"}");
  EXPECT_EQ(FindLatest(root, &found, &why), Status::kCorrupt);

  // A manifest that names another checkpoint.
  WriteText(root + "/latest", PointerJson("step-1"));
  Manifest other;
  other.checkpoint = "step-2";
  WriteText(directory + "/manifest.json", ToJson(other));
  EXPECT_EQ(FindLatest(root, &found, &why), Status::kCorrupt);
  EXPECT_NE(why.find("names step-2"), std::string::npos) << why;
}

TEST(CheckpointStore, NamesThatCouldEscapeAreRefused) {
  const std::string root = Root("names");
  std::string directory, why;
  for (const char* bad : {"..", "a/b", "latest", "x.tmp", ""}) {
    EXPECT_EQ(PrepareCheckpoint(root, bad, &directory, &why),
              Status::kBadSchema)
        << bad;
    EXPECT_EQ(CommitCheckpoint(root, bad, 0, &why), Status::kBadSchema) << bad;
  }
}

}  // namespace
}  // namespace abmio
