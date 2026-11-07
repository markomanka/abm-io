// SPDX-License-Identifier: Apache-2.0
// The types every checkpoint carries beside its agents, and the comparison
// that decides whether a run may resume from one.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "abmio/records.h"
#include "abmio/schema.h"
#include "abmio/visit.h"

namespace abmio {
namespace {

ComponentRecord Component(const std::string& name, const std::string& version,
                          bool affects, const std::string& digest = "") {
  ComponentRecord c;
  c.name = name;
  c.version = version;
  c.role = "library";
  c.digest = digest;
  c.affects_results = affects;
  return c;
}

TEST(Records, TheyAreDescribedLikeAnyOtherType) {
  Schema s;
  s.types = {Describe<SimulationRecord>(), Describe<RandomStreamRecord>(),
             Describe<ProvenanceRecord>(), Describe<ComponentRecord>()};
  std::string why;
  EXPECT_EQ(Validate(s, &why), Status::kOk) << why;

  const TypeDescription sim = Describe<SimulationRecord>();
  ASSERT_EQ(sim.fields.size(), 3u);
  EXPECT_EQ(sim.fields[0].element, ElementType::kUInt64);   // step
  EXPECT_EQ(sim.fields[1].element, ElementType::kFloat64);  // time
  EXPECT_EQ(sim.fields[2].kind, Kind::kString);             // time_unit

  const TypeDescription component = Describe<ComponentRecord>();
  ASSERT_EQ(component.fields.size(), 6u);
  EXPECT_EQ(component.qualified_name, "abmio.Component");
  EXPECT_EQ(component.version, 1u);
}

struct Names {
  std::vector<std::string> names;
  template <class M>
  void operator()(const Field& f, M&) {
    names.emplace_back(f.name);
  }
};

TEST(Records, EveryFieldIsVisitedByName) {
  Names names;
  ProvenanceRecord p;
  VisitFields<ProvenanceRecord>(names, p, Role::kCheckpoint);
  const std::vector<std::string> expected = {
      "abmio_version", "schema_digest",      "seed",
      "compression",   "compression_inputs", "created"};
  EXPECT_EQ(names.names, expected);
}

TEST(Components, AnUnchangedRunMayResume) {
  const std::vector<ComponentRecord> before = {
      Component("biodynamo", "1.06", true),
      Component("paraview", "6.2", false)};
  std::vector<std::string> differences;
  EXPECT_FALSE(ComponentsDiffer(before, before, &differences));
  EXPECT_TRUE(differences.empty());
}

TEST(Components, OnlyWhatAffectsResultsStopsAResume) {
  const std::vector<ComponentRecord> before = {
      Component("solver", "2.1", true), Component("paraview", "6.2", false)};
  // A visualisation library that moved on does not stop a restart.
  std::vector<ComponentRecord> now = before;
  now[1].version = "6.3";
  std::vector<std::string> differences;
  EXPECT_FALSE(ComponentsDiffer(before, now, &differences))
      << differences.size();

  // A solver that moved on does.
  now = before;
  now[0].version = "2.2";
  EXPECT_TRUE(ComponentsDiffer(before, now, &differences));
  ASSERT_EQ(differences.size(), 1u);
  EXPECT_NE(differences[0].find("was 2.1, is now 2.2"), std::string::npos)
      << differences[0];
}

TEST(Components, EveryDifferenceIsReportedAndNotOnlyTheFirst) {
  const std::vector<ComponentRecord> before = {Component("a", "1", true),
                                               Component("b", "1", true),
                                               Component("c", "1", true)};
  std::vector<ComponentRecord> now = before;
  now[0].version = "2";
  now[1].version = "3";
  now.pop_back();  // c is gone
  std::vector<std::string> differences;
  EXPECT_TRUE(ComponentsDiffer(before, now, &differences));
  EXPECT_EQ(differences.size(), 3u);
}

TEST(Components, SameVersionDifferentContentIsADifference) {
  const std::vector<ComponentRecord> before = {
      Component("weights", "1.0", true, "sha256:aaa")};
  std::vector<ComponentRecord> now = before;
  now[0].digest = "sha256:bbb";
  std::vector<std::string> differences;
  EXPECT_TRUE(ComponentsDiffer(before, now, &differences));
  ASSERT_EQ(differences.size(), 1u);
  EXPECT_NE(differences[0].find("different content"), std::string::npos)
      << differences[0];
  // Without digests on both sides there is nothing to compare, and the
  // versions agree, so the resume is allowed.
  now[0].digest.clear();
  EXPECT_FALSE(ComponentsDiffer(before, now, &differences));
}

TEST(Components, SomethingNewThatAffectsResultsIsADifference) {
  const std::vector<ComponentRecord> before = {
      Component("solver", "2.1", true)};
  std::vector<ComponentRecord> now = before;
  now.push_back(Component("gene network", "0.9", true));
  std::vector<std::string> differences;
  EXPECT_TRUE(ComponentsDiffer(before, now, &differences));
  ASSERT_EQ(differences.size(), 1u);
  EXPECT_NE(differences[0].find("was not present"), std::string::npos)
      << differences[0];
  // One that does not affect results may appear freely.
  now.back().affects_results = false;
  EXPECT_FALSE(ComponentsDiffer(before, now, &differences));
}

TEST(Components, AcceptanceIsPerComponentAndNotABlanketFlag) {
  const std::vector<ComponentRecord> before = {
      Component("solver", "2.1", true), Component("gene network", "0.9", true)};
  std::vector<ComponentRecord> now = before;
  now[0].version = "2.2";  // a fix that changes results, knowingly taken
  now[1].version = "1.0";

  // Nothing accepted: both differences stop the resume.
  std::vector<std::string> blocking;
  EXPECT_TRUE(BlockingDifferences(before, now, {}, &blocking));
  EXPECT_EQ(blocking.size(), 2u);

  // One accepted by name: the other still stops it.
  blocking.clear();
  EXPECT_TRUE(BlockingDifferences(before, now, {"solver"}, &blocking));
  ASSERT_EQ(blocking.size(), 1u);
  EXPECT_NE(blocking[0].find("gene network"), std::string::npos) << blocking[0];

  // Both accepted by name: the resume may go ahead.
  blocking.clear();
  EXPECT_FALSE(
      BlockingDifferences(before, now, {"solver", "gene network"}, &blocking));
  EXPECT_TRUE(blocking.empty());

  // A name that matches nothing accepts nothing.
  EXPECT_TRUE(BlockingDifferences(before, now, {"paraview"}, &blocking));
}

TEST(Components, WhatDoesNotAffectResultsNeedsNoAcceptance) {
  const std::vector<ComponentRecord> before = {
      Component("paraview", "6.2", false)};
  std::vector<ComponentRecord> now = before;
  now[0].version = "6.3";
  std::vector<std::string> blocking;
  EXPECT_FALSE(BlockingDifferences(before, now, {}, &blocking));
}

TEST(Lineage, ADiscontinuityIsDescribedLikeAnyOtherType) {
  Schema s;
  s.types = {Describe<LineageRecord>()};
  std::string why;
  EXPECT_EQ(Validate(s, &why), Status::kOk) << why;
  const TypeDescription d = Describe<LineageRecord>();
  EXPECT_EQ(d.qualified_name, "abmio.Lineage");
  ASSERT_EQ(d.fields.size(), 7u);
  EXPECT_EQ(d.fields[0].element,
            ElementType::kUInt64);  // the step it happened at
}

}  // namespace
}  // namespace abmio
