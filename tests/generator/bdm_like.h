// SPDX-License-Identifier: Apache-2.0
// A miniature of BioDynaMo for testing abm-gen: its agent and behaviour macros
// (with the friend line BioDynaMo's step 3 adds), its own member types, and
// classes written the way BioDynaMo writes them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "abmio/schema.h"

#define BDM_AGENT_HEADER(class_name, base_class, class_version_id) \
 public:                                                           \
  using Base = base_class;                                         \
                                                                   \
 private:                                                          \
  template <class>                                                 \
  friend struct ::abmio::TypeTraits

#define BDM_BEHAVIOR_HEADER(class_name, base_class, class_version_id) \
  BDM_AGENT_HEADER(class_name, base_class, class_version_id)

namespace bdm {

template <class T, std::size_t N>
struct MathArray {
  T data[N];
  T& operator[](std::size_t i) { return data[i]; }
};
using Real3 = MathArray<double, 3>;

struct AgentUid {
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

struct Spinlock {  // cannot be described: must stay transient
  int flag = 0;
};

class Behavior {
  template <class>
  friend struct ::abmio::TypeTraits;

 public:
  virtual ~Behavior() = default;

 private:
  uint32_t copy_mask_ = 0;
  static int instances_;
};

class Agent {
  template <class>
  friend struct ::abmio::TypeTraits;

 public:
  virtual ~Agent() = default;

 private:
  AgentUid uid_;
  uint32_t box_idx_ = 0;
  InlineVector<Behavior*, 2> behaviors_;
  Spinlock lock_;           //!
  bool is_static_ = false;  //!
};

class Cell : public Agent {
  BDM_AGENT_HEADER(Cell, Agent, 4);

 private:
  Real3 position_{};
  double diameter_ = 0;
  double volume_ = 0;
  AgentPointer<Cell> mother_;
  static constexpr int kDimensions = 3;
};

class GrowthDivision : public Behavior {
  BDM_BEHAVIOR_HEADER(GrowthDivision, Behavior, 2);

 private:
  double threshold_ = 40;
  double growth_rate_ = 300;
};

}  // namespace bdm
