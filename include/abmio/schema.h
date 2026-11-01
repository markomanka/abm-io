// SPDX-License-Identifier: Apache-2.0
// The schema (docs/INTEGRATION.md, "The schema"): descriptions of agent and
// behaviour types, their fields and the relations between agents, and the
// visitor protocol through which a simulator exposes its types.
//
// A simulator describes each type once, by specialising TypeTraits:
//
//   template <>
//   struct abmio::TypeTraits<bdm::Cell> {
//     static constexpr const char* kName = "bdm::Cell";
//     static constexpr uint32_t kVersion = 1;
//     using Base = bdm::Agent;  // or void
//     template <class V>
//     static void Visit(V& v) {
//       v(Field("position").Unit("um"), &bdm::Cell::position_);
//       v(Field("mother").Reference(), &bdm::Cell::mother_uid_);
//     }
//   };
//
// Visit passes pointers to members, never values, so Describe() derives a
// type's description without constructing an object, and the library's data
// visitors bind the object themselves. Optional members of TypeTraits: kId (an
// explicit type identifier, kept when the type is renamed) and kTerm (an
// ontology term).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include "abmio/status.h"

namespace abmio {

// ---------------------------------------------------------------------------
// Identifiers

// A type identifier is FNV-1a 64 over the bytes of the qualified name, so any
// tool (such as the generator) computes the same value. 0 means "no type".
using TypeId = uint64_t;
inline constexpr TypeId kNoType = 0;

constexpr TypeId TypeIdOf(std::string_view qualified_name) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (char c : qualified_name) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// An agent identifier is unique across processes for the whole run and never
// reused by abm-io. 0 means "no agent", as in an empty parent column.
using AgentId = uint64_t;
inline constexpr AgentId kNoAgent = 0;

// Hands out agent identifiers without coordination: process `rank` of `ranks`
// takes first + rank, first + rank + ranks, and so on. After a restart, pass
// one more than the largest identifier in the checkpoint as `first`; the number
// of processes may differ. Requires rank < ranks and first > 0.
class AgentIdAllocator {
 public:
  AgentIdAllocator(uint32_t rank, uint32_t ranks, AgentId first = 1)
      : next_(first + rank), stride_(ranks) {}
  AgentId Next() {
    const AgentId id = next_;
    next_ += stride_;
    return id;
  }
  AgentId Peek() const { return next_; }

 private:
  AgentId next_;
  uint64_t stride_;
};

// ---------------------------------------------------------------------------
// Fields

// What a field is stored or sent for. A field without roles is never stored or
// sent, like a cached neighbour list.
enum class Role : uint8_t {
  kNone = 0,
  kCheckpoint = 1,
  kExchange = 2,
  kExport = 4,
  kAll = 7
};

constexpr Role operator|(Role a, Role b) {
  return static_cast<Role>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

// True if `set` includes every role in `r`; false for Role::kNone.
constexpr bool HasRole(Role set, Role r) {
  return r != Role::kNone &&
         (static_cast<uint8_t>(set) & static_cast<uint8_t>(r)) ==
             static_cast<uint8_t>(r);
}

// Element types, with their precision explicit. kAgentId marks references to
// agents, stored as 64-bit agent identifiers.
enum class ElementType : uint8_t {
  kNone,
  kBool,
  kChar,
  kInt8,
  kUInt8,
  kInt16,
  kUInt16,
  kInt32,
  kUInt32,
  kInt64,
  kUInt64,
  kFloat32,
  kFloat64,
  kAgentId
};

std::size_t SizeOf(ElementType t);
const char* ToString(ElementType t);

enum class Kind : uint8_t {
  kScalar,          // one element
  kArray,           // a fixed number of elements, such as a 3D position
  kVector,          // a variable number of elements
  kString,          // characters
  kRecord,          // a nested described type
  kPolymorphicList  // a list of objects of described types sharing a base, such
                    // as behaviours
};

const char* ToString(Kind k);

// How a visit function names a field. Builder methods chain:
// Field("diameter").Unit("um").StoredAs(ElementType::kFloat32).
struct Field {
  constexpr explicit Field(std::string_view field_name) : name(field_name) {}
  constexpr Field& Roles(Role r) {
    roles = r;
    return *this;
  }
  constexpr Field& Unit(std::string_view u) {
    unit = u;
    return *this;
  }
  constexpr Field& Term(std::string_view t) {
    term = t;
    return *this;
  }
  // Stores the field with a narrower element type of the same category, such as
  // float32 for a float64 member. abm-io never converts units, only precision.
  constexpr Field& StoredAs(ElementType t) {
    stored = t;
    return *this;
  }
  // The member holds agent identifiers (64-bit unsigned integers).
  constexpr Field& Reference() {
    reference = true;
    return *this;
  }

  std::string_view name;
  Role roles = Role::kAll;
  std::string_view unit;
  std::string_view term;
  ElementType stored = ElementType::kNone;  // kNone: as in memory
  bool reference = false;
};

struct FieldDescription {
  std::string name;
  Kind kind = Kind::kScalar;
  ElementType element =
      ElementType::kNone;  // kNone for records and polymorphic lists
  uint32_t count = 1;      // elements of a scalar or array; 0 otherwise
  ElementType stored = ElementType::kNone;  // kNone: as `element`
  TypeId record_type = kNoType;             // records and polymorphic lists
  Role roles = Role::kAll;
  std::string unit;
  std::string term;
};

struct TypeDescription {
  std::string qualified_name;
  TypeId id = kNoType;
  TypeId base = kNoType;  // kNoType: no base type
  uint32_t version = 1;
  std::string term;
  std::vector<FieldDescription>
      fields;  // this type's own fields; the base's come first
};

// A relation between agents, stored as an edge table: source and target agent
// identifiers, then the attribute fields. Lineage needs no relation: a
// reference field, such as a parent, is a column.
struct RelationDescription {
  std::string name;
  std::string term;
  std::vector<FieldDescription> attributes;
};

struct Schema {
  std::vector<TypeDescription> types;
  std::vector<RelationDescription> relations;

  const TypeDescription* Find(TypeId id) const;
};

// Checks a schema: unique type names and identifiers (a collision is an error),
// existing base types without cycles, unique field names within each type and
// its bases, consistent kinds and counts, stored types of the same category,
// existing record types, and unique relation names. On failure returns
// Status::kBadSchema and, if `why` is given, a description of the first
// problem.
Status Validate(const Schema& schema, std::string* why = nullptr);

// ---------------------------------------------------------------------------
// The visitor protocol

// Specialised by a simulator's adapter (or generated by abm-gen) for each type.
template <class T>
struct TypeTraits;

namespace detail {

template <class T, class = void>
struct IsDescribed : std::false_type {};
template <class T>
struct IsDescribed<T, std::void_t<decltype(TypeTraits<T>::kName)>>
    : std::true_type {};

template <class T, class = void>
struct HasExplicitId : std::false_type {};
template <class T>
struct HasExplicitId<T, std::void_t<decltype(TypeTraits<T>::kId)>>
    : std::true_type {};

template <class T, class = void>
struct HasTerm : std::false_type {};
template <class T>
struct HasTerm<T, std::void_t<decltype(TypeTraits<T>::kTerm)>>
    : std::true_type {};

template <class T>
constexpr ElementType ElementOf() {
  if constexpr (std::is_same_v<T, bool>) {
    return ElementType::kBool;
  } else if constexpr (std::is_same_v<T, char>) {
    return ElementType::kChar;
  } else if constexpr (std::is_floating_point_v<T>) {
    return sizeof(T) == 4 ? ElementType::kFloat32 : ElementType::kFloat64;
  } else if constexpr (std::is_integral_v<T>) {
    constexpr bool kSigned = std::is_signed_v<T>;
    if constexpr (sizeof(T) == 1) {
      return kSigned ? ElementType::kInt8 : ElementType::kUInt8;
    } else if constexpr (sizeof(T) == 2) {
      return kSigned ? ElementType::kInt16 : ElementType::kUInt16;
    } else if constexpr (sizeof(T) == 4) {
      return kSigned ? ElementType::kInt32 : ElementType::kUInt32;
    } else {
      return kSigned ? ElementType::kInt64 : ElementType::kUInt64;
    }
  } else {
    return ElementType::kNone;
  }
}

template <class T>
constexpr bool kIsElement = std::is_arithmetic_v<T> && sizeof(T) <= 8;

template <class T>
struct Unpointer {
  using type = void;
};
template <class B>
struct Unpointer<B*> {
  using type = B;
};
template <class B>
struct Unpointer<std::unique_ptr<B>> {
  using type = B;
};

template <class T>
constexpr TypeId IdOf();

template <class M>
struct IsStdArray : std::false_type {};
template <class E, std::size_t N>
struct IsStdArray<std::array<E, N>> : std::true_type {};

template <class M>
struct IsStdVector : std::false_type {};
template <class E, class A>
struct IsStdVector<std::vector<E, A>> : std::true_type {};

template <class M>
FieldDescription ShapeOfContainer() {
  FieldDescription d;
  if constexpr (IsStdArray<M>::value && kIsElement<typename M::value_type>) {
    d.kind = Kind::kArray;
    d.element = ElementOf<typename M::value_type>();
    d.count = static_cast<uint32_t>(std::tuple_size_v<M>);
  } else if constexpr (IsStdVector<M>::value &&
                       kIsElement<typename M::value_type>) {
    d.kind = Kind::kVector;
    d.element = ElementOf<typename M::value_type>();
    d.count = 0;
  } else if constexpr (IsStdVector<M>::value &&
                       IsDescribed<typename Unpointer<
                           typename M::value_type>::type>::value) {
    d.kind = Kind::kPolymorphicList;
    d.count = 0;
    d.record_type = IdOf<typename Unpointer<typename M::value_type>::type>();
  } else {
    static_assert(
        sizeof(M) == 0,
        "abm-io cannot describe this member type: use an arithmetic type, a "
        "fixed array, std::array, std::vector, std::string, a described type, "
        "or "
        "a std::vector of pointers to a described type");
  }
  return d;
}

// Shape of a member type: kind, element type, count and record type.
template <class M>
FieldDescription ShapeOf() {
  FieldDescription d;
  if constexpr (kIsElement<M>) {
    d.kind = Kind::kScalar;
    d.element = ElementOf<M>();
  } else if constexpr (std::is_array_v<M> && std::rank_v<M> == 1 &&
                       kIsElement<std::remove_extent_t<M>>) {
    d.kind = Kind::kArray;
    d.element = ElementOf<std::remove_extent_t<M>>();
    d.count = static_cast<uint32_t>(std::extent_v<M>);
  } else if constexpr (std::is_same_v<M, std::string>) {
    d.kind = Kind::kString;
    d.element = ElementType::kChar;
    d.count = 0;
  } else if constexpr (IsDescribed<M>::value) {
    d.kind = Kind::kRecord;
    d.count = 0;
    d.record_type = IdOf<M>();
  } else {
    d = ShapeOfContainer<M>();
  }
  return d;
}

template <class T>
constexpr TypeId IdOf() {
  if constexpr (HasExplicitId<T>::value) {
    return TypeTraits<T>::kId;
  } else {
    return TypeIdOf(TypeTraits<T>::kName);
  }
}

class Describer {
 public:
  explicit Describer(std::vector<FieldDescription>* out) : out_(out) {}

  template <class M, class C>
  void operator()(const Field& f, M C::*) {
    FieldDescription d = ShapeOf<M>();
    d.name = std::string(f.name);
    d.roles = f.roles;
    d.unit = std::string(f.unit);
    d.term = std::string(f.term);
    d.stored = f.stored;
    if (f.reference) {
      // Agent identifiers are 64-bit unsigned; on any other member the field
      // gets no element type, which Validate() reports.
      d.element = d.element == ElementType::kUInt64 ? ElementType::kAgentId
                                                    : ElementType::kNone;
    }
    out_->push_back(std::move(d));
  }

 private:
  std::vector<FieldDescription>* out_;
};

}  // namespace detail

// The description of type T, from TypeTraits<T>.
template <class T>
TypeDescription Describe() {
  using Traits = TypeTraits<T>;
  TypeDescription d;
  d.qualified_name = Traits::kName;
  d.id = detail::IdOf<T>();
  d.version = Traits::kVersion;
  if constexpr (!std::is_void_v<typename Traits::Base>) {
    d.base = detail::IdOf<typename Traits::Base>();
  }
  if constexpr (detail::HasTerm<T>::value) {
    d.term = Traits::kTerm;
  }
  detail::Describer describer(&d.fields);
  Traits::Visit(describer);
  return d;
}

// The fields of type T alone, for example as the attributes of a relation.
template <class T>
std::vector<FieldDescription> DescribeFields() {
  return Describe<T>().fields;
}

}  // namespace abmio
