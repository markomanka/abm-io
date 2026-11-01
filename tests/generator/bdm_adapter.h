// SPDX-License-Identifier: Apache-2.0
// The miniature BioDynaMo's own member types, declared once for abm-io, as
// BioDynaMo's adapter will declare them.
#pragma once
#include <cstddef>
#include <cstdint>

#include "abmio/schema.h"
#include "bdm_like.h"

namespace abmio {

template <class T, std::size_t N>
struct FieldTraits<bdm::MathArray<T, N>> {
  static constexpr Kind kKind = Kind::kArray;
  using Element = T;
  static constexpr uint32_t kCount = N;
  static T& At(bdm::MathArray<T, N>& m, std::size_t i) { return m[i]; }
};

template <>
struct FieldTraits<bdm::AgentUid> {
  static constexpr Kind kKind = Kind::kScalar;
  using Element = uint64_t;
  static constexpr bool kReference = true;
  static uint64_t Load(const bdm::AgentUid& u) {
    return (uint64_t{u.reused} << 32) | u.index;
  }
  static void Store(bdm::AgentUid& u, uint64_t id) {
    u.index = static_cast<uint32_t>(id);
    u.reused = static_cast<uint32_t>(id >> 32);
  }
};

template <class T>
struct FieldTraits<bdm::AgentPointer<T>> {
  static constexpr Kind kKind = Kind::kScalar;
  using Element = uint64_t;
  static constexpr bool kReference = true;
  static uint64_t Load(const bdm::AgentPointer<T>& p) {
    return FieldTraits<bdm::AgentUid>::Load(p.uid);
  }
  static void Store(bdm::AgentPointer<T>& p, uint64_t id) {
    FieldTraits<bdm::AgentUid>::Store(p.uid, id);
  }
};

template <class T, std::size_t N>
struct FieldTraits<bdm::InlineVector<T*, N>> {
  static constexpr Kind kKind = Kind::kPolymorphicList;
  using Element = T;
};

}  // namespace abmio
