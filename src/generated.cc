// SPDX-License-Identifier: Apache-2.0
#include "abmio/generated.h"

namespace abmio::gen {
namespace {
struct Sizes {
  std::vector<uint32_t> s;
  template <class T>
  void operator()(T&) {
    s.push_back(static_cast<uint32_t>(sizeof(T)));
  }
};
template <class T, class F>
std::vector<uint32_t> SizesOf(F visit) {
  Sizes s;
  T obj;
  visit(s, obj);
  return s.s;
}
const std::vector<uint32_t> kNone;
}  // namespace

Agent* NewAgent(int t) {
  switch (t) {
    case 1:
      return new Cell;
    case 2:
      return new MyCell;
    default:
      return nullptr;
  }
}
Behavior* NewBehavior(int t) {
  switch (t) {
    case 1:
      return new Growth;
    case 2:
      return new Secretion;
    default:
      return nullptr;
  }
}
const std::vector<uint32_t>& AgentFieldSizes(int t) {
  static const auto cell =
      SizesOf<Cell>([](Sizes& s, Cell& c) { VisitCell(s, c); });
  static const auto mycell =
      SizesOf<MyCell>([](Sizes& s, MyCell& c) { VisitMyCell(s, c); });
  return t == 1 ? cell : t == 2 ? mycell : kNone;
}
const std::vector<uint32_t>& BehaviorFieldSizes(int t) {
  static const auto growth =
      SizesOf<Growth>([](Sizes& s, Growth& g) { VisitGrowth(s, g); });
  static const auto secretion =
      SizesOf<Secretion>([](Sizes& s, Secretion& x) { VisitSecretion(s, x); });
  return t == 1 ? growth : t == 2 ? secretion : kNone;
}

}  // namespace abmio::gen
