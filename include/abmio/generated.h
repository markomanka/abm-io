// SPDX-License-Identifier: Apache-2.0
// What abm-gen would emit for the test model: field visitors, factories and
// field-size tables. Hand-written here until the generator exists (Phase 2).
#pragma once
#include <cstdint>
#include <vector>
#include "abmio/model.h"

namespace abmio::gen {

constexpr int kNumAgentTypes = 2;     // type ids 1..kNumAgentTypes
constexpr int kNumBehaviorTypes = 2;  // type ids 1..kNumBehaviorTypes

template <class V>
void VisitAgent(V& v, Agent& a) {
  v(a.uid);
  v(a.box_idx);
}
template <class V>
void VisitCell(V& v, Cell& c) {
  VisitAgent(v, c);
  for (double& x : c.pos)
    v(x);
  for (double& x : c.tractor)
    v(x);
  v(c.diameter);
  v(c.volume);
  v(c.adherence);
  v(c.density);
  v(c.mother_uid);
}
template <class V>
void VisitMyCell(V& v, MyCell& c) {
  VisitCell(v, c);
  v(c.cell_type);
  v(c.age);
}
template <class V>
void VisitGrowth(V& v, Growth& g) {
  v(g.threshold);
  v(g.rate);
}
template <class V>
void VisitSecretion(V& v, Secretion& s) {
  v(s.substance);
  v(s.quantity);
}

// Dispatch on a type id whose dynamic type the caller guarantees.
template <class V>
bool VisitAgentById(V& v, Agent& a, int t) {
  switch (t) {
    case 1:
      VisitCell(v, static_cast<Cell&>(a));
      return true;
    case 2:
      VisitMyCell(v, static_cast<MyCell&>(a));
      return true;
    default:
      return false;
  }
}
template <class V>
bool VisitBehaviorById(V& v, Behavior& b, int t) {
  switch (t) {
    case 1:
      VisitGrowth(v, static_cast<Growth&>(b));
      return true;
    case 2:
      VisitSecretion(v, static_cast<Secretion&>(b));
      return true;
    default:
      return false;
  }
}

Agent* NewAgent(int t);        // nullptr for unknown ids
Behavior* NewBehavior(int t);  // nullptr for unknown ids
const std::vector<uint32_t>& AgentFieldSizes(int t);
const std::vector<uint32_t>& BehaviorFieldSizes(int t);

}  // namespace abmio::gen
