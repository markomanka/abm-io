// SPDX-License-Identifier: Apache-2.0
// Type descriptions of the test model (abmio/model.h), in the form abm-gen
// will generate for a simulator: one TypeTraits specialisation per agent and
// behaviour type. The exchange packs the fields with the exchange role through
// VisitElements (abmio/visit.h) and lays them out with ElementSizes, so the
// descriptions alone decide what travels between processes. The order and
// sizes match the hand-written visitors these replace, so the message format
// is unchanged.
#pragma once
#include <cstdint>
#include <vector>
#include "abmio/model.h"
#include "abmio/schema.h"
#include "abmio/visit.h"

namespace abmio {

template <>
struct TypeTraits<Agent> {
  static constexpr const char* kName = "abmio::Agent";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("uid").Reference(), &Agent::uid);
    // The index of the agent's box in the simulator's grid. Transient in a
    // simulator, but part of the current message format, so it travels.
    v(Field("box_idx"), &Agent::box_idx);
  }
};

template <>
struct TypeTraits<Cell> {
  static constexpr const char* kName = "abmio::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = Agent;
  template <class V>
  static void Visit(V& v) {
    v(Field("pos").Unit("um"), &Cell::pos);
    v(Field("tractor"), &Cell::tractor);
    v(Field("diameter").Unit("um"), &Cell::diameter);
    v(Field("volume").Unit("um3"), &Cell::volume);
    v(Field("adherence"), &Cell::adherence);
    v(Field("density"), &Cell::density);
    v(Field("mother_uid").Reference(), &Cell::mother_uid);
  }
};

template <>
struct TypeTraits<MyCell> {
  static constexpr const char* kName = "abmio::MyCell";
  static constexpr uint32_t kVersion = 1;
  using Base = Cell;
  template <class V>
  static void Visit(V& v) {
    v(Field("cell_type"), &MyCell::cell_type);
    v(Field("age"), &MyCell::age);
  }
};

template <>
struct TypeTraits<Behavior> {
  static constexpr const char* kName = "abmio::Behavior";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V&) {}
};

template <>
struct TypeTraits<Growth> {
  static constexpr const char* kName = "abmio::Growth";
  static constexpr uint32_t kVersion = 1;
  using Base = Behavior;
  template <class V>
  static void Visit(V& v) {
    v(Field("threshold"), &Growth::threshold);
    v(Field("rate"), &Growth::rate);
  }
};

template <>
struct TypeTraits<Secretion> {
  static constexpr const char* kName = "abmio::Secretion";
  static constexpr uint32_t kVersion = 1;
  using Base = Behavior;
  template <class V>
  static void Visit(V& v) {
    v(Field("substance"), &Secretion::substance);
    v(Field("quantity"), &Secretion::quantity);
  }
};

namespace gen {

// The message format numbers types 1..kNum*Types, in this order.
constexpr int kNumAgentTypes = 2;     // 1: Cell, 2: MyCell
constexpr int kNumBehaviorTypes = 2;  // 1: Growth, 2: Secretion

// Calls v(element) for each exchanged element of `a`, whose dynamic type the
// caller guarantees to be agent type `t`. False for unknown types.
template <class V>
bool VisitAgentById(V& v, Agent& a, int t) {
  switch (t) {
    case 1:
      VisitElements<Cell>(v, static_cast<Cell&>(a), Role::kExchange);
      return true;
    case 2:
      VisitElements<MyCell>(v, static_cast<MyCell&>(a), Role::kExchange);
      return true;
    default:
      return false;
  }
}

template <class V>
bool VisitBehaviorById(V& v, Behavior& b, int t) {
  switch (t) {
    case 1:
      VisitElements<Growth>(v, static_cast<Growth&>(b), Role::kExchange);
      return true;
    case 2:
      VisitElements<Secretion>(v, static_cast<Secretion&>(b), Role::kExchange);
      return true;
    default:
      return false;
  }
}

Agent* NewAgent(int t);        // nullptr for unknown ids
Behavior* NewBehavior(int t);  // nullptr for unknown ids
// Sizes of the exchanged elements, from the type descriptions.
const std::vector<uint32_t>& AgentFieldSizes(int t);
const std::vector<uint32_t>& BehaviorFieldSizes(int t);
// The descriptions of the model's types, for validation and for files.
Schema ModelSchema();

}  // namespace gen
}  // namespace abmio
