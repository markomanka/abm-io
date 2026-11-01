// SPDX-License-Identifier: Apache-2.0
// The customisation point for a simulator's own member types, on a miniature
// of BioDynaMo's: MathArray (Real3), AgentUid, AgentPointer and InlineVector.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "abmio/schema.h"
#include "abmio/visit.h"

namespace sim {
template <class T, std::size_t N>
struct MathArray {
  T data[N];
  T& operator[](std::size_t i) { return data[i]; }
};
using Real3 = MathArray<double, 3>;

struct AgentUid {  // an index and a reuse counter, 64 bits together
  uint32_t index = 0;
  uint32_t reused = 0;
};

template <class T>
struct AgentPointer {
  AgentUid uid;
};

template <class T, std::size_t N>
struct InlineVector {
  std::vector<T> items;
};

struct Behavior {
  virtual ~Behavior() = default;
};

struct Agent {
  AgentUid uid_;
  InlineVector<Behavior*, 2> behaviors_;
};

struct Cell : Agent {
  Real3 position_{};
  double diameter_ = 0;
  AgentPointer<Cell> mother_;
};
}  // namespace sim

namespace abmio {

// Declared once per simulator, in its adapter.
template <class T, std::size_t N>
struct FieldTraits<sim::MathArray<T, N>> {
  static constexpr Kind kKind = Kind::kArray;
  using Element = T;
  static constexpr uint32_t kCount = N;
  static T& At(sim::MathArray<T, N>& m, std::size_t i) { return m[i]; }
};

template <>
struct FieldTraits<sim::AgentUid> {
  static constexpr Kind kKind = Kind::kScalar;
  using Element = uint64_t;
  static constexpr bool kReference = true;
  static uint64_t Load(const sim::AgentUid& u) {
    return (uint64_t{u.reused} << 32) | u.index;
  }
  static void Store(sim::AgentUid& u, uint64_t id) {
    u.index = static_cast<uint32_t>(id);
    u.reused = static_cast<uint32_t>(id >> 32);
  }
};

template <class T>
struct FieldTraits<sim::AgentPointer<T>> {
  static constexpr Kind kKind = Kind::kScalar;
  using Element = uint64_t;
  static constexpr bool kReference = true;
  static uint64_t Load(const sim::AgentPointer<T>& p) {
    return FieldTraits<sim::AgentUid>::Load(p.uid);
  }
  static void Store(sim::AgentPointer<T>& p, uint64_t id) {
    FieldTraits<sim::AgentUid>::Store(p.uid, id);
  }
};

template <class T, std::size_t N>
struct FieldTraits<sim::InlineVector<T*, N>> {
  static constexpr Kind kKind = Kind::kPolymorphicList;
  using Element = T;
};

// What abm-gen generates for the classes.
template <>
struct TypeTraits<sim::Behavior> {
  static constexpr const char* kName = "sim::Behavior";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V&) {}
};

template <>
struct TypeTraits<sim::Agent> {
  static constexpr const char* kName = "sim::Agent";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("uid"), &sim::Agent::uid_);
    v(Field("behaviors"), &sim::Agent::behaviors_);
  }
};

template <>
struct TypeTraits<sim::Cell> {
  static constexpr const char* kName = "sim::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = sim::Agent;
  template <class V>
  static void Visit(V& v) {
    v(Field("position").Unit("um"), &sim::Cell::position_);
    v(Field("diameter").Unit("um"), &sim::Cell::diameter_);
    v(Field("mother"), &sim::Cell::mother_);
  }
};

namespace {

struct Recorder {  // records sizes and numbers the elements 1, 2, 3, ...
  std::vector<uint32_t> sizes;
  template <class T>
  void operator()(T& x) {
    sizes.push_back(static_cast<uint32_t>(sizeof(T)));
    x = static_cast<T>(sizes.size());
  }
};

struct Copier {  // reads every element, writes nothing
  std::vector<double> values;
  template <class T>
  void operator()(T& x) {
    values.push_back(static_cast<double>(x));
  }
};

TEST(FieldTraits, DescribeSimulatorTypes) {
  const TypeDescription agent = Describe<sim::Agent>();
  ASSERT_EQ(agent.fields.size(), 2u);
  EXPECT_EQ(agent.fields[0].kind, Kind::kScalar);
  EXPECT_EQ(agent.fields[0].element, ElementType::kAgentId);
  EXPECT_EQ(agent.fields[1].kind, Kind::kPolymorphicList);
  EXPECT_EQ(agent.fields[1].record_type, TypeIdOf("sim::Behavior"));

  const TypeDescription cell = Describe<sim::Cell>();
  ASSERT_EQ(cell.fields.size(), 3u);
  EXPECT_EQ(cell.fields[0].kind, Kind::kArray);
  EXPECT_EQ(cell.fields[0].element, ElementType::kFloat64);
  EXPECT_EQ(cell.fields[0].count, 3u);
  EXPECT_EQ(cell.fields[0].unit, "um");
  EXPECT_EQ(cell.fields[1].kind, Kind::kScalar);
  EXPECT_EQ(cell.fields[2].element, ElementType::kAgentId);

  Schema s;
  s.types = {Describe<sim::Behavior>(), agent, cell};
  std::string why;
  EXPECT_EQ(Validate(s, &why), Status::kOk) << why;
}

TEST(FieldTraits, ElementsSkipTheBehaviourList) {
  // uid, position x3, diameter, mother: the behaviour list travels separately.
  const std::vector<uint32_t> expected = {8, 8, 8, 8, 8, 8};
  EXPECT_EQ(ElementSizes<sim::Cell>(Role::kExchange), expected);
  sim::Cell c;
  Recorder r;
  VisitElements<sim::Cell>(r, c, Role::kExchange);
  EXPECT_EQ(r.sizes, expected);
}

TEST(FieldTraits, ReferencesAndArraysWriteThrough) {
  sim::Cell c;
  Recorder r;
  VisitElements<sim::Cell>(r, c, Role::kExchange);  // numbers the elements 1..6
  EXPECT_EQ(c.uid_.index, 1u);
  EXPECT_EQ(c.uid_.reused, 0u);
  EXPECT_EQ(c.position_[0], 2.0);
  EXPECT_EQ(c.position_[2], 4.0);
  EXPECT_EQ(c.diameter_, 5.0);
  EXPECT_EQ(c.mother_.uid.index, 6u);
}

TEST(FieldTraits, ReadingLeavesReferencesUnchanged) {
  sim::Cell c;
  c.uid_ = {7, 3};
  c.mother_.uid = {9, 1};
  c.position_ = {{1.5, 2.5, 3.5}};
  Copier copy;
  VisitElements<sim::Cell>(copy, c, Role::kExchange);
  ASSERT_EQ(copy.values.size(), 6u);
  EXPECT_EQ(copy.values[0], static_cast<double>((uint64_t{3} << 32) | 7));
  EXPECT_EQ(copy.values[1], 1.5);
  EXPECT_EQ(c.uid_.index, 7u);  // Store(m, Load(m)) leaves m unchanged
  EXPECT_EQ(c.uid_.reused, 3u);
  EXPECT_EQ(c.mother_.uid.index, 9u);
  EXPECT_EQ(c.mother_.uid.reused, 1u);
}

}  // namespace
}  // namespace abmio
