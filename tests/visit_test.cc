// SPDX-License-Identifier: Apache-2.0
#include "abmio/visit.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "abmio/generated.h"

namespace probe {
struct Probe {
  double a = 0;
  int32_t b = 0;
  std::array<float, 2> c{};
};
}  // namespace probe

namespace abmio {
template <>
struct TypeTraits<probe::Probe> {
  static constexpr const char* kName = "probe::Probe";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("a"), &probe::Probe::a);
    v(Field("b").Roles(Role::kCheckpoint), &probe::Probe::b);
    v(Field("c").Roles(Role::kExchange | Role::kExport), &probe::Probe::c);
  }
};
}  // namespace abmio

namespace abmio {
namespace {

// Records the size of each element visited, and numbers the elements.
struct Recorder {
  std::vector<uint32_t> sizes;
  template <class T>
  void operator()(T& x) {
    sizes.push_back(static_cast<uint32_t>(sizeof(T)));
    x = static_cast<T>(sizes.size());
  }
};

struct Names {
  std::vector<std::string> names;
  template <class M>
  void operator()(const Field& f, M&) {
    names.emplace_back(f.name);
  }
};

template <class T>
std::vector<uint32_t> VisitedSizes() {
  T obj;
  Recorder r;
  VisitElements<T>(r, obj, Role::kExchange);
  return r.sizes;
}

TEST(Visit, ExchangeLayoutIsTheEstablishedMessageFormat) {
  // The field sizes of the hand-written visitors the descriptions replace.
  const std::vector<uint32_t> cell = {8, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
  std::vector<uint32_t> mycell = cell;
  mycell.insert(mycell.end(), {4, 8});
  EXPECT_EQ(gen::AgentFieldSizes(1), cell);
  EXPECT_EQ(gen::AgentFieldSizes(2), mycell);
  EXPECT_EQ(gen::BehaviorFieldSizes(1), (std::vector<uint32_t>{8, 8}));
  EXPECT_EQ(gen::BehaviorFieldSizes(2), (std::vector<uint32_t>{4, 8}));
  EXPECT_TRUE(gen::AgentFieldSizes(3).empty());
  EXPECT_TRUE(gen::BehaviorFieldSizes(0).empty());
}

TEST(Visit, DescriptionsAgreeWithTheDataVisitors) {
  EXPECT_EQ(ElementSizes<Cell>(Role::kExchange), VisitedSizes<Cell>());
  EXPECT_EQ(ElementSizes<MyCell>(Role::kExchange), VisitedSizes<MyCell>());
  EXPECT_EQ(ElementSizes<Growth>(Role::kExchange), VisitedSizes<Growth>());
  EXPECT_EQ(ElementSizes<Secretion>(Role::kExchange),
            VisitedSizes<Secretion>());
  EXPECT_EQ(ElementSizes<probe::Probe>(Role::kExchange),
            VisitedSizes<probe::Probe>());
}

TEST(Visit, BaseTypesFirstInDeclarationOrder) {
  MyCell c;
  Names n;
  VisitFields<MyCell>(n, c, Role::kExchange);
  const std::vector<std::string> expected = {
      "uid",       "box_idx", "pos",        "tractor",   "diameter", "volume",
      "adherence", "density", "mother_uid", "cell_type", "age"};
  EXPECT_EQ(n.names, expected);
}

TEST(Visit, RolesSelectTheFields) {
  probe::Probe p;
  Names exchanged, checkpointed;
  VisitFields<probe::Probe>(exchanged, p, Role::kExchange);
  VisitFields<probe::Probe>(checkpointed, p, Role::kCheckpoint);
  EXPECT_EQ(exchanged.names, (std::vector<std::string>{"a", "c"}));
  EXPECT_EQ(checkpointed.names, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(ElementSizes<probe::Probe>(Role::kExchange),
            (std::vector<uint32_t>{8, 4, 4}));
  EXPECT_EQ(ElementSizes<probe::Probe>(Role::kCheckpoint),
            (std::vector<uint32_t>{8, 4}));
  EXPECT_EQ(ElementSizes<probe::Probe>(Role::kExport | Role::kExchange),
            (std::vector<uint32_t>{8, 4, 4}));
}

TEST(Visit, VisitorsWriteThroughToTheObject) {
  MyCell c;
  Recorder r;
  gen::VisitAgentById(r, c, 2);  // numbers the elements 1, 2, 3, ...
  EXPECT_EQ(c.uid, 1u);
  EXPECT_EQ(c.box_idx, 2u);
  EXPECT_EQ(c.pos[0], 3.0);
  EXPECT_EQ(c.pos[2], 5.0);
  EXPECT_EQ(c.mother_uid, 13u);
  EXPECT_EQ(c.cell_type, 14);
  EXPECT_EQ(c.age, 15.0);
  probe::Probe p;
  Recorder rp;
  VisitElements<probe::Probe>(rp, p, Role::kExchange);
  EXPECT_EQ(p.c[1], 3.0f);
  EXPECT_EQ(p.b, 0);  // not exchanged, untouched
  Recorder unknown;
  EXPECT_FALSE(gen::VisitAgentById(unknown, c, 9));
  Growth g;
  EXPECT_FALSE(gen::VisitBehaviorById(unknown, g, 9));
  EXPECT_TRUE(unknown.sizes.empty());
}

TEST(Visit, FactoriesCreateEachTypeAndRefuseUnknownOnes) {
  for (int t = 1; t <= gen::kNumAgentTypes; ++t) {
    std::unique_ptr<Agent> a(gen::NewAgent(t));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->TypeId(), t);
  }
  for (int t = 1; t <= gen::kNumBehaviorTypes; ++t) {
    std::unique_ptr<Behavior> b(gen::NewBehavior(t));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->TypeId(), t);
  }
  EXPECT_EQ(gen::NewAgent(0), nullptr);
  EXPECT_EQ(gen::NewBehavior(gen::kNumBehaviorTypes + 1), nullptr);
}

TEST(Visit, TheModelSchemaIsValid) {
  const Schema s = gen::ModelSchema();
  std::string why;
  EXPECT_EQ(Validate(s, &why), Status::kOk) << why;
  ASSERT_EQ(s.types.size(), 6u);
  EXPECT_EQ(s.Find(TypeIdOf("abmio::MyCell"))->base, TypeIdOf("abmio::Cell"));
  EXPECT_EQ(s.Find(TypeIdOf("abmio::Growth"))->base,
            TypeIdOf("abmio::Behavior"));
}

}  // namespace
}  // namespace abmio
