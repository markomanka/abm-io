// SPDX-License-Identifier: Apache-2.0
// Data visitors on top of the schema's visitor protocol (abmio/schema.h).
// VisitFields binds an object to its type's visit function, base types first;
// VisitElements turns each fixed-size field into one call per element, the
// form in which the exchange packs and unpacks agents; ElementSizes gives the
// matching sizes from the type's description, without an object. Only roles
// select fields: units are never converted, and a narrower stored type
// applies to files, not to the exchange.
#pragma once
#include <cstdint>
#include <type_traits>
#include <vector>
#include "abmio/schema.h"

namespace abmio {
namespace detail {

template <class V, class T>
class Binder {
 public:
  Binder(V& v, T& obj, Role roles) : v_(v), obj_(obj), roles_(roles) {}

  template <class M, class C>
  void operator()(const Field& f, M C::* member) {
    if (HasRole(f.roles, roles_)) {
      v_(f, obj_.*member);
    }
  }

 private:
  V& v_;
  T& obj_;
  Role roles_;
};

template <class V>
class PerElement {
 public:
  explicit PerElement(V& v) : v_(v) {}

  template <class M>
  void operator()(const Field&, M& member) {
    if constexpr (kIsElement<M>) {
      v_(member);
    } else if constexpr (std::is_array_v<M> || IsStdArray<M>::value) {
      for (auto& element : member) {
        v_(element);
      }
    } else {
      static_assert(sizeof(M) == 0,
                    "VisitElements handles fixed-size fields only: scalars and "
                    "fixed arrays");
    }
  }

 private:
  V& v_;
};

}  // namespace detail

// Calls v(field, member) for each field of `obj` that has all of `roles`: the
// fields of its base types first, then its own, in declaration order.
template <class T, class V>
void VisitFields(V& v, T& obj, Role roles) {
  using Base = typename TypeTraits<T>::Base;
  if constexpr (!std::is_void_v<Base>) {
    VisitFields<Base>(v, static_cast<Base&>(obj), roles);
  }
  detail::Binder<V, T> binder(v, obj, roles);
  TypeTraits<T>::Visit(binder);
}

// Calls v(element) for each element of the fields VisitFields would visit.
template <class T, class V>
void VisitElements(V& v, T& obj, Role roles) {
  detail::PerElement<V> per_element(v);
  VisitFields<T>(per_element, obj, roles);
}

// The size in bytes of each element VisitElements visits, in the same order,
// from the descriptions of T and its base types.
template <class T>
std::vector<uint32_t> ElementSizes(Role roles) {
  std::vector<uint32_t> sizes;
  using Base = typename TypeTraits<T>::Base;
  if constexpr (!std::is_void_v<Base>) {
    sizes = ElementSizes<Base>(roles);
  }
  for (const FieldDescription& f : Describe<T>().fields) {
    if (!HasRole(f.roles, roles)) {
      continue;
    }
    for (uint32_t i = 0; i < f.count; ++i) {
      sizes.push_back(static_cast<uint32_t>(SizeOf(f.element)));
    }
  }
  return sizes;
}

}  // namespace abmio
