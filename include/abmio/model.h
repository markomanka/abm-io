// SPDX-License-Identifier: Apache-2.0
// Test agent model shaped like BioDynaMo's Agent -> Cell -> user subclass.
// It stands in for real BioDynaMo classes until abm-gen generates code for
// them.
#pragma once
#include <cstdint>
#include <vector>

namespace abmio {

struct Behavior {
  virtual ~Behavior() = default;
  virtual uint8_t TypeId() const { return 0; }
};
struct Growth : Behavior {
  double threshold = 40, rate = 300;
  uint8_t TypeId() const override { return 1; }
};
struct Secretion : Behavior {
  int substance = 0;
  double quantity = 1;
  uint8_t TypeId() const override { return 2; }
};

struct Agent {
  Agent() = default;
  Agent(const Agent&) = delete;
  Agent& operator=(const Agent&) = delete;
  virtual ~Agent() {
    for (auto* b : behaviors)
      delete b;
  }
  virtual uint8_t TypeId() const { return 0; }
  uint64_t uid = 0;
  uint32_t box_idx = 0;
  std::vector<Behavior*> behaviors;  // owned
};
struct Cell : Agent {
  uint8_t TypeId() const override { return 1; }
  double pos[3] = {0, 0, 0};
  double tractor[3] = {0, 0, 0};
  double diameter = 10, volume = 0, adherence = 0.1, density = 1;
  uint64_t mother_uid = 0;
};
struct MyCell : Cell {
  uint8_t TypeId() const override { return 2; }
  int cell_type = 0;
  double age = 0;
};

}  // namespace abmio
