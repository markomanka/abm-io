// SPDX-License-Identifier: Apache-2.0
#include "abmio/generated.h"

namespace abmio::gen {
namespace {
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
  static const auto cell = ElementSizes<Cell>(Role::kExchange);
  static const auto mycell = ElementSizes<MyCell>(Role::kExchange);
  return t == 1 ? cell : t == 2 ? mycell : kNone;
}

const std::vector<uint32_t>& BehaviorFieldSizes(int t) {
  static const auto growth = ElementSizes<Growth>(Role::kExchange);
  static const auto secretion = ElementSizes<Secretion>(Role::kExchange);
  return t == 1 ? growth : t == 2 ? secretion : kNone;
}

Schema ModelSchema() {
  Schema s;
  s.types = {Describe<Agent>(),    Describe<Cell>(),   Describe<MyCell>(),
             Describe<Behavior>(), Describe<Growth>(), Describe<Secretion>()};
  return s;
}

}  // namespace abmio::gen
