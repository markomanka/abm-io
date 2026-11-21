// SPDX-License-Identifier: Apache-2.0
// The strict resume's check of dependencies: a changed component that affects
// results stops a resume unless accepted by name, and an accepted change
// becomes a lineage row that later checkpoints carry (docs/RATIONALE.md,
// entry 11).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "abmio/checkpoint.h"
#include "abmio/records.h"

namespace abmio {
namespace {

std::string Path(const std::string& name) {
  return ::testing::TempDir() + "abmio_resume_" + name + ".h5";
}

ComponentRecord Component(const std::string& name, const std::string& version,
                          bool affects_results = true) {
  ComponentRecord c;
  c.name = name;
  c.version = version;
  c.role = affects_results ? "solver" : "library";
  c.affects_results = affects_results;
  return c;
}

std::vector<ComponentRecord> Recorded() {
  return {Component("biodynamo", "1.07"), Component("libroadrunner", "2.5.0"),
          Component("paraview", "5.13", false)};
}

// A checkpoint at step 500 with the components above and one lineage row
// from an earlier accepted change.
void WriteCheckpoint(const std::string& path) {
  std::string why;
  CheckpointWriter writer;
  ASSERT_EQ(writer.Open(path, {}, &why), Status::kOk) << why;
  SimulationRecord sim;
  sim.step = 500;
  sim.time_unit = "min";
  ASSERT_EQ(writer.WriteRecord(kSimulationRecord, sim, &why), Status::kOk)
      << why;
  auto components = Recorded();
  ASSERT_EQ(writer.WriteTable<ComponentRecord>(
                kComponentsTable, components.size(),
                [&components](uint64_t i) -> ComponentRecord& {
                  return components[i];
                },
                &why),
            Status::kOk)
      << why;
  std::vector<LineageRecord> lineage(1);
  lineage[0].step = 200;
  lineage[0].component = "biodynamo";
  lineage[0].from_version = "1.06";
  lineage[0].to_version = "1.07";
  lineage[0].accepted_by = "earlier";
  lineage[0].reason = "flocking fix";
  ASSERT_EQ(writer.WriteTable<LineageRecord>(
                kLineageTable, lineage.size(),
                [&lineage](uint64_t i) -> LineageRecord& { return lineage[i]; },
                &why),
            Status::kOk)
      << why;
  ASSERT_EQ(writer.Close(&why), Status::kOk) << why;
}

Status Check(const std::string& path, const std::vector<ComponentRecord>& now,
             const ResumeAcceptance& acceptance,
             std::vector<LineageRecord>* lineage, std::string* why) {
  CheckpointReader reader;
  Status s = reader.Open(path, {}, why);
  return s == Status::kOk ? reader.CheckResume(now, acceptance, lineage, why)
                          : s;
}

TEST(StrictResume, TheSameComponentsResumeAndCarryTheLineage) {
  const std::string path = Path("same");
  WriteCheckpoint(path);
  std::vector<LineageRecord> lineage;
  std::string why;
  auto now = Recorded();
  now[2].version = "6.0";  // does not affect results
  ASSERT_EQ(Check(path, now, {}, &lineage, &why), Status::kOk) << why;
  ASSERT_EQ(lineage.size(), 1u);
  EXPECT_EQ(lineage[0].component, "biodynamo");
  EXPECT_EQ(lineage[0].step, 200u);
}

TEST(StrictResume, EveryUnacceptedChangeIsNamed) {
  const std::string path = Path("changed");
  WriteCheckpoint(path);
  auto now = Recorded();
  now[1].version = "2.6.0";
  now.push_back(Component("gene-network", "0.1"));
  std::vector<LineageRecord> lineage;
  std::string why;
  EXPECT_EQ(Check(path, now, {}, &lineage, &why), Status::kBadSchema);
  EXPECT_NE(why.find("libroadrunner (solver) was 2.5.0, is now 2.6.0"),
            std::string::npos)
      << why;
  EXPECT_NE(why.find("gene-network"), std::string::npos) << why;
  EXPECT_TRUE(lineage.empty());

  // A prefix accepts nothing.
  ResumeAcceptance prefix{{"libroad", "gene-network"}, "test", "prefix", ""};
  EXPECT_EQ(Check(path, now, prefix, &lineage, &why), Status::kBadSchema);
  EXPECT_NE(why.find("libroadrunner"), std::string::npos) << why;
  EXPECT_EQ(why.find("gene-network (solver) 0.1"), std::string::npos) << why;
}

TEST(StrictResume, AcceptedChangesBecomeLineageRows) {
  const std::string path = Path("accepted");
  WriteCheckpoint(path);
  auto now = Recorded();
  now[1].version = "2.6.0";
  ResumeAcceptance acceptance{{"libroadrunner", "paraview"},
                              "Marco",
                              "2.5.0 integrated stiff systems wrongly",
                              "2026-11-21T12:00:00Z"};
  std::vector<LineageRecord> lineage;
  std::string why;
  ASSERT_EQ(Check(path, now, acceptance, &lineage, &why), Status::kOk) << why;
  ASSERT_EQ(lineage.size(), 2u) << "paraview did not change: no row";
  EXPECT_EQ(lineage[0].component, "biodynamo");  // carried
  const LineageRecord& added = lineage[1];
  EXPECT_EQ(added.step, 500u);
  EXPECT_EQ(added.component, "libroadrunner");
  EXPECT_EQ(added.from_version, "2.5.0");
  EXPECT_EQ(added.to_version, "2.6.0");
  EXPECT_EQ(added.accepted_by, "Marco");
  EXPECT_EQ(added.reason, "2.5.0 integrated stiff systems wrongly");
  EXPECT_EQ(added.at, "2026-11-21T12:00:00Z");
}

TEST(StrictResume, AcceptanceSaysWhoAndWhy) {
  const std::string path = Path("anonymous");
  WriteCheckpoint(path);
  auto now = Recorded();
  now[1].version = "2.6.0";
  std::vector<LineageRecord> lineage;
  std::string why;
  EXPECT_EQ(
      Check(path, now, {{"libroadrunner"}, "", "fixed", ""}, &lineage, &why),
      Status::kBadSchema);
  EXPECT_NE(why.find("who accepts it and why"), std::string::npos) << why;
  EXPECT_EQ(
      Check(path, now, {{"libroadrunner"}, "Marco", "", ""}, &lineage, &why),
      Status::kBadSchema);
}

TEST(StrictResume, ACheckpointWithoutComponentsCannotBeResumed) {
  const std::string path = Path("bare");
  std::string why;
  CheckpointWriter writer;
  ASSERT_EQ(writer.Open(path, {}, &why), Status::kOk) << why;
  SimulationRecord sim;
  ASSERT_EQ(writer.WriteRecord(kSimulationRecord, sim, &why), Status::kOk);
  ASSERT_EQ(writer.Close(&why), Status::kOk) << why;
  std::vector<LineageRecord> lineage;
  EXPECT_NE(Check(path, Recorded(), {}, &lineage, &why), Status::kOk);
  EXPECT_NE(why.find("cannot resume"), std::string::npos) << why;
  EXPECT_NE(why.find("components"), std::string::npos) << why;
}

}  // namespace
}  // namespace abmio
