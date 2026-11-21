// SPDX-License-Identifier: Apache-2.0
// Data visitors on top of the schema's visitor protocol (abmio/schema.h).
// VisitFields binds an object to its type's visit function, base types first;
// VisitElements turns each fixed-size field into one call per element, the
// form in which the exchange packs and unpacks agents, and skips variable-size
// fields, records and lists, which travel separately (behaviours, for
// instance, as records of their own); ElementSizes gives the matching sizes
// from the type's description, without an object. Only roles select fields:
// units are never converted, and a narrower stored type applies to
// visualization files only, never to checkpoints or the exchange.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
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
    if constexpr (HasFieldTraits<M>::value) {
      VisitDeclared(member);
    } else if constexpr (kIsElement<M>) {
      v_(member);
    } else if constexpr (std::is_array_v<M> || IsStdArray<M>::value) {
      for (auto& element : member) {
        v_(element);
      }
    }
    // Anything else (vectors, strings, records, lists) has no fixed size and
    // is skipped, as ElementSizes skips it.
  }

 private:
  template <class M>
  void VisitDeclared(M& member) {
    using Traits = FieldTraits<M>;
    if constexpr (IsFieldReference<M>::value) {
      uint64_t id = Traits::Load(member);
      v_(id);
      Traits::Store(member, id);
    } else if constexpr (Traits::kKind == Kind::kScalar) {
      v_(Traits::At(member, 0));
    } else if constexpr (Traits::kKind == Kind::kArray) {
      for (std::size_t i = 0; i < Traits::kCount; ++i) {
        v_(Traits::At(member, i));
      }
    }
  }

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

// ---------------------------------------------------------------------------
// Lists of behaviours (Kind::kPolymorphicList)
//
// A list holds objects of several described types behind pointers to a common
// base, as a simulator's behaviours do. The walk below is the same for every
// destination, a checkpoint table or an exchange message: for each element,
// its type identifier and the object as its concrete type, whose fields the
// caller then visits like any other object's. Building a list back takes a
// type identifier, makes an object of that type, lets the caller fill it, and
// appends it.
//
// The base's TypeTraits names the concrete types that may appear:
//   using Subtypes = std::tuple<Grow, Divide>;
// Each is described, derives from the base, and is default-constructible; the
// base is polymorphic, so that an element's own type can be told. An element
// of a type not listed is refused with the list's position named, never
// written as its base. abm-gen lists the subtypes it finds in the headers.
//
// The list may be a std::vector of raw pointers or of std::unique_ptr, or a
// simulator's own container described by FieldTraits with
//   kKind = Kind::kPolymorphicList, Element = the base,
//   static std::size_t Size(const M&); static Element* At(M&, std::size_t);
//   static void Append(M&, Element*);   // takes the new object over
// A raw-pointer std::vector owns nothing here: Append pushes `new T`, and the
// simulator deletes as it always does.

namespace detail {

template <class L, class = void>
struct ListOf {};

template <class P>
struct ListOf<std::vector<P>,
              std::enable_if_t<!std::is_void_v<typename Unpointer<P>::type>>> {
  using Base = typename Unpointer<P>::type;
  static std::size_t Size(const std::vector<P>& l) { return l.size(); }
  static Base* At(std::vector<P>& l, std::size_t i) {
    if constexpr (std::is_pointer_v<P>) {
      return l[i];
    } else {
      return l[i].get();
    }
  }
  static void Append(std::vector<P>& l, Base* e) { l.emplace_back(e); }
};

template <class L>
struct ListOf<L, std::enable_if_t<HasFieldTraits<L>::value>> {
  using Traits = FieldTraits<L>;
  static_assert(Traits::kKind == Kind::kPolymorphicList,
                "FieldTraits: this member is not a polymorphic list");
  using Base = typename Traits::Element;
  static std::size_t Size(const L& l) { return Traits::Size(l); }
  static Base* At(L& l, std::size_t i) { return Traits::At(l, i); }
  static void Append(L& l, Base* e) { Traits::Append(l, e); }
};

template <class Base>
using SubtypesOf = typename TypeTraits<Base>::Subtypes;

template <class Base, std::size_t... I>
constexpr bool SubtypesValid(std::index_sequence<I...>) {
  using List = SubtypesOf<Base>;
  return (... &&
          (std::is_base_of_v<Base, std::tuple_element_t<I, List>> &&
           std::is_default_constructible_v<std::tuple_element_t<I, List>> &&
           IsDescribed<std::tuple_element_t<I, List>>::value));
}

template <class Base, std::size_t... I>
constexpr bool SubtypeIdsDistinct(std::index_sequence<I...>) {
  using List = SubtypesOf<Base>;
  constexpr TypeId ids[] = {IdOf<std::tuple_element_t<I, List>>()...};
  for (std::size_t a = 0; a < sizeof...(I); ++a) {
    for (std::size_t b = a + 1; b < sizeof...(I); ++b) {
      if (ids[a] == ids[b]) {
        return false;
      }
    }
  }
  return true;
}

template <class Base>
constexpr void CheckSubtypes() {
  static_assert(std::is_polymorphic_v<Base>,
                "a polymorphic list's base must have a virtual function, so "
                "that each element's own type can be told");
  constexpr std::size_t n = std::tuple_size_v<SubtypesOf<Base>>;
  static_assert(SubtypesValid<Base>(std::make_index_sequence<n>()),
                "each of TypeTraits<Base>::Subtypes must be described, derive "
                "from the base and be default-constructible");
  static_assert(SubtypeIdsDistinct<Base>(std::make_index_sequence<n>()),
                "two of TypeTraits<Base>::Subtypes have the same type "
                "identifier");
}

// Calls f(id, object as T&) for the listed T that is the element's own type.
template <class Base, class F, std::size_t... I>
bool Dispatch(Base* e, F& f, std::index_sequence<I...>) {
  using List = SubtypesOf<Base>;
  bool found = false;
  (void)((typeid(*e) == typeid(std::tuple_element_t<I, List>)
              ? (f(IdOf<std::tuple_element_t<I, List>>(),
                   *dynamic_cast<std::tuple_element_t<I, List>*>(e)),
                 found = true)
              : false) ||
         ...);
  return found;
}

template <class T, class F, class A>
void MakeOne(F& f, A& append) {
  std::unique_ptr<T> made(new T());
  f(*made);
  append(made.release());
}

// Makes a T of the listed type with identifier `id`, lets f fill it, and
// hands it to `append`.
template <class Base, class F, class A, std::size_t... I>
bool Make(TypeId id, F& f, A& append, std::index_sequence<I...>) {
  using List = SubtypesOf<Base>;
  bool found = false;
  (void)((IdOf<std::tuple_element_t<I, List>>() == id
              ? (MakeOne<std::tuple_element_t<I, List>>(f, append),
                 found = true)
              : false) ||
         ...);
  return found;
}

}  // namespace detail

/// Calls f(id, element) for each element of a polymorphic list, in order,
/// with the element as its own listed type and its type identifier. Returns
/// false, with `why` naming the position, at the first element that is null
/// or of a type not in TypeTraits<Base>::Subtypes; elements before it have
/// been visited.
template <class L, class F>
bool ForEachElement(L& list, F&& f, std::string* why) {
  using Access = detail::ListOf<std::remove_const_t<L>>;
  using Base = typename Access::Base;
  detail::CheckSubtypes<Base>();
  constexpr std::size_t n = std::tuple_size_v<detail::SubtypesOf<Base>>;
  const std::size_t size = Access::Size(list);
  for (std::size_t i = 0; i < size; ++i) {
    Base* e = Access::At(list, i);
    const char* problem = nullptr;
    if (e == nullptr) {
      problem = " is null";
    } else if (!detail::Dispatch<Base>(e, f, std::make_index_sequence<n>())) {
      problem = " is of a type not listed in the base's Subtypes";
    }
    if (problem != nullptr) {
      if (why != nullptr) {
        *why = std::string("element ") + std::to_string(i) + " of a list of " +
               TypeTraits<Base>::kName + problem;
      }
      return false;
    }
  }
  return true;
}

/// Appends a new element of the listed type with identifier `id` to a
/// polymorphic list, after f(element) has filled it as its own type. Returns
/// false, with `why` naming the identifier, if no listed type has it, as when
/// a file holds a behaviour this build does not know.
template <class L, class F>
bool AppendElement(L& list, TypeId id, F&& f, std::string* why) {
  using Access = detail::ListOf<L>;
  using Base = typename Access::Base;
  detail::CheckSubtypes<Base>();
  constexpr std::size_t n = std::tuple_size_v<detail::SubtypesOf<Base>>;
  auto append = [&list](Base* e) { Access::Append(list, e); };
  if (detail::Make<Base>(id, f, append, std::make_index_sequence<n>())) {
    return true;
  }
  if (why != nullptr) {
    *why = "no type listed in " + std::string(TypeTraits<Base>::kName) +
           "'s Subtypes has the identifier " + std::to_string(id);
  }
  return false;
}

}  // namespace abmio
