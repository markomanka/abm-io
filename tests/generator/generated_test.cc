// SPDX-License-Identifier: Apache-2.0
// Compiles abm-gen's output for the miniature BioDynaMo and checks that the
// descriptions are valid and drive the data visitors, private members included.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "abmio/visit.h"
#include "generated.h"

namespace abmio {
namespace {

struct Names {
  std::vector<std::string> names;
  template <class M>
  void operator()(const Field& f, M&) {
    names.emplace_back(f.name);
  }
};

struct Recorder {
  std::vector<uint32_t> sizes;
  template <class T>
  void operator()(T& x) {
    sizes.push_back(static_cast<uint32_t>(sizeof(T)));
    x = static_cast<T>(sizes.size());
  }
};

TEST(Generated, TheDescriptionsFormAValidSchema) {
  Schema s;
  s.types = {Describe<bdm::Agent>(), Describe<bdm::Cell>(),
             Describe<bdm::Behavior>(), Describe<bdm::GrowthDivision>(),
             Describe<model::MyCell>()};
  std::string why;
  EXPECT_EQ(Validate(s, &why), Status::kOk) << why;
  EXPECT_EQ(s.Find(TypeIdOf("model::MyCell"))->base, TypeIdOf("bdm::Cell"));
  EXPECT_EQ(s.Find(TypeIdOf("bdm::Cell"))->version, 4u);
}

TEST(Generated, SimulatorTypesAreDescribed) {
  const TypeDescription agent = Describe<bdm::Agent>();
  ASSERT_EQ(agent.fields.size(),
            3u);  // the lock and the static flag are transient
  EXPECT_EQ(agent.fields[0].element, ElementType::kAgentId);
  EXPECT_EQ(agent.fields[2].kind, Kind::kPolymorphicList);
  const TypeDescription cell = Describe<bdm::Cell>();
  EXPECT_EQ(cell.fields[0].kind, Kind::kArray);
  EXPECT_EQ(cell.fields[0].count, 3u);
  EXPECT_EQ(cell.fields[0].unit, "um");
  EXPECT_EQ(cell.fields[3].element, ElementType::kAgentId);
  const TypeDescription mycell = Describe<model::MyCell>();
  ASSERT_EQ(mycell.fields.size(), 2u);
  EXPECT_EQ(mycell.fields[1].kind, Kind::kVector);
  EXPECT_EQ(mycell.fields[1].roles, Role::kCheckpoint | Role::kExport);
}

TEST(Generated, VisitorsReachPrivateMembers) {
  model::MyCell c;
  Names n;
  VisitFields<model::MyCell>(n, c, Role::kExchange);
  const std::vector<std::string> expected = {
      "uid",      "box_idx", "behaviors", "position",
      "diameter", "volume",  "mother",    "cell_type"};
  EXPECT_EQ(n.names, expected);  // concentrations has no exchange role
  Recorder r;
  VisitElements<model::MyCell>(r, c, Role::kExchange);
  EXPECT_EQ(r.sizes, ElementSizes<model::MyCell>(Role::kExchange));
}

}  // namespace
}  // namespace abmio
