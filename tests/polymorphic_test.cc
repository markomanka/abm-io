// SPDX-License-Identifier: Apache-2.0
// The walk over lists of behaviours: each element is given as its own type
// with its type identifier, whatever the destination, and a list is built back
// from identifiers and fields; unknown types and null elements are refused.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "abmio/schema.h"
#include "abmio/visit.h"

namespace poly_test {

struct Behaviour {
  virtual ~Behaviour() = default;
  bool copy_to_new = true;
};

struct Grow : Behaviour {
  double rate = 0;
};

struct Divide : Behaviour {
  int32_t max_divisions = 0;
  double threshold[2] = {0, 0};
};

struct Unlisted : Behaviour {};

// A simulator's own container of behaviours.
struct BehaviourList {
  std::vector<Behaviour*> items;
  ~BehaviourList() {
    for (Behaviour* b : items) {
      delete b;
    }
  }
};

}  // namespace poly_test

namespace abmio {

template <>
struct TypeTraits<poly_test::Behaviour> {
  static constexpr const char* kName = "poly_test::Behaviour";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  using Subtypes = std::tuple<poly_test::Grow, poly_test::Divide>;
  template <class V>
  static void Visit(V& v) {
    v(Field("copy_to_new"), &poly_test::Behaviour::copy_to_new);
  }
};

template <>
struct TypeTraits<poly_test::Grow> {
  static constexpr const char* kName = "poly_test::Grow";
  static constexpr uint32_t kVersion = 1;
  using Base = poly_test::Behaviour;
  template <class V>
  static void Visit(V& v) {
    v(Field("rate"), &poly_test::Grow::rate);
  }
};

template <>
struct TypeTraits<poly_test::Divide> {
  static constexpr const char* kName = "poly_test::Divide";
  static constexpr uint32_t kVersion = 1;
  using Base = poly_test::Behaviour;
  template <class V>
  static void Visit(V& v) {
    v(Field("max_divisions"), &poly_test::Divide::max_divisions);
    v(Field("threshold"), &poly_test::Divide::threshold);
  }
};

template <>
struct FieldTraits<poly_test::BehaviourList> {
  static constexpr Kind kKind = Kind::kPolymorphicList;
  using Element = poly_test::Behaviour;
  static std::size_t Size(const poly_test::BehaviourList& l) {
    return l.items.size();
  }
  static poly_test::Behaviour* At(poly_test::BehaviourList& l, std::size_t i) {
    return l.items[i];
  }
  static void Append(poly_test::BehaviourList& l, poly_test::Behaviour* b) {
    l.items.push_back(b);
  }
};

namespace {

using poly_test::Behaviour;
using poly_test::Divide;
using poly_test::Grow;

// A destination of the simplest kind: each element as its identifier and the
// bytes of its fields, base first, as an exchange message would carry it.
struct Encoded {
  TypeId id;
  std::vector<char> bytes;
};

struct Append {
  std::vector<char>* out;
  template <class X>
  void operator()(X& x) {
    const char* p = reinterpret_cast<const char*>(&x);
    out->insert(out->end(), p, p + sizeof(X));
  }
};

struct Take {
  const std::vector<char>* in;
  std::size_t at = 0;
  template <class X>
  void operator()(X& x) {
    std::memcpy(&x, in->data() + at, sizeof(X));
    at += sizeof(X);
  }
};

template <class L>
std::vector<Encoded> Encode(L& list, bool* ok, std::string* why) {
  std::vector<Encoded> out;
  *ok = ForEachElement(
      list,
      [&out](TypeId id, auto& element) {
        using T = std::remove_reference_t<decltype(element)>;
        Encoded e{id, {}};
        Append append{&e.bytes};
        VisitElements<T>(append, element, Role::kCheckpoint);
        out.push_back(e);
      },
      why);
  return out;
}

template <class L>
bool Decode(const std::vector<Encoded>& in, L& list, std::string* why) {
  for (const Encoded& e : in) {
    const bool ok = AppendElement(
        list, e.id,
        [&e](auto& element) {
          using T = std::remove_reference_t<decltype(element)>;
          Take take{&e.bytes};
          VisitElements<T>(take, element, Role::kCheckpoint);
        },
        why);
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::vector<std::unique_ptr<Behaviour>> Sample() {
  std::vector<std::unique_ptr<Behaviour>> l;
  auto g = std::make_unique<Grow>();
  g->rate = 0.25;
  g->copy_to_new = false;
  auto d = std::make_unique<Divide>();
  d->max_divisions = 3;
  d->threshold[0] = 8;
  d->threshold[1] = 12.5;
  auto g2 = std::make_unique<Grow>();
  g2->rate = -1;
  l.push_back(std::move(g));
  l.push_back(std::move(d));
  l.push_back(std::move(g2));
  return l;
}

template <class L>
void ExpectSample(L& list, std::size_t (*size)(L&),
                  Behaviour* (*at)(L&, std::size_t)) {
  ASSERT_EQ(size(list), 3u);
  auto* g = dynamic_cast<Grow*>(at(list, 0));
  auto* d = dynamic_cast<Divide*>(at(list, 1));
  auto* g2 = dynamic_cast<Grow*>(at(list, 2));
  ASSERT_NE(g, nullptr);
  ASSERT_NE(d, nullptr);
  ASSERT_NE(g2, nullptr);
  EXPECT_EQ(g->rate, 0.25);
  EXPECT_FALSE(g->copy_to_new);
  EXPECT_EQ(d->max_divisions, 3);
  EXPECT_EQ(d->threshold[0], 8);
  EXPECT_EQ(d->threshold[1], 12.5);
  EXPECT_TRUE(d->copy_to_new);
  EXPECT_EQ(g2->rate, -1);
}

TEST(PolymorphicList, EachElementComesAsItsOwnTypeWithItsIdentifier) {
  auto list = Sample();
  std::vector<TypeId> ids;
  std::vector<std::string> names;
  std::string why;
  ASSERT_TRUE(ForEachElement(
      list,
      [&](TypeId id, auto& element) {
        using T = std::remove_reference_t<decltype(element)>;
        ids.push_back(id);
        names.push_back(TypeTraits<T>::kName);
      },
      &why))
      << why;
  EXPECT_EQ(names,
            (std::vector<std::string>{"poly_test::Grow", "poly_test::Divide",
                                      "poly_test::Grow"}));
  EXPECT_EQ(ids[0], detail::IdOf<Grow>());
  EXPECT_EQ(ids[1], detail::IdOf<Divide>());
}

TEST(PolymorphicList, ABuiltBackListHoldsTheSameObjects) {
  auto list = Sample();
  bool ok = false;
  std::string why;
  const auto encoded = Encode(list, &ok, &why);
  ASSERT_TRUE(ok) << why;

  // Into unique pointers, raw pointers and a simulator's own container.
  std::vector<std::unique_ptr<Behaviour>> owned;
  ASSERT_TRUE(Decode(encoded, owned, &why)) << why;
  ExpectSample<std::vector<std::unique_ptr<Behaviour>>>(
      owned,
      [](std::vector<std::unique_ptr<Behaviour>>& l) { return l.size(); },
      [](std::vector<std::unique_ptr<Behaviour>>& l, std::size_t i) {
        return l[i].get();
      });

  std::vector<Behaviour*> raw;
  ASSERT_TRUE(Decode(encoded, raw, &why)) << why;
  ExpectSample<std::vector<Behaviour*>>(
      raw, [](std::vector<Behaviour*>& l) { return l.size(); },
      [](std::vector<Behaviour*>& l, std::size_t i) { return l[i]; });
  for (Behaviour* b : raw) {
    delete b;
  }

  poly_test::BehaviourList own;
  ASSERT_TRUE(Decode(encoded, own, &why)) << why;
  ExpectSample<poly_test::BehaviourList>(
      own, [](poly_test::BehaviourList& l) { return l.items.size(); },
      [](poly_test::BehaviourList& l, std::size_t i) { return l.items[i]; });

  // And the simulator's container walks like the others.
  const auto again = Encode(own, &ok, &why);
  ASSERT_TRUE(ok) << why;
  ASSERT_EQ(again.size(), encoded.size());
  for (std::size_t i = 0; i < again.size(); ++i) {
    EXPECT_EQ(again[i].id, encoded[i].id);
    EXPECT_EQ(again[i].bytes, encoded[i].bytes);
  }
}

TEST(PolymorphicList, AnUnlistedTypeOrANullElementIsRefusedByPosition) {
  std::vector<std::unique_ptr<Behaviour>> list;
  list.push_back(std::make_unique<Grow>());
  list.push_back(std::make_unique<poly_test::Unlisted>());
  int visited = 0;
  std::string why;
  EXPECT_FALSE(
      ForEachElement(list, [&visited](TypeId, auto&) { ++visited; }, &why));
  EXPECT_EQ(visited, 1);
  EXPECT_NE(why.find("element 1"), std::string::npos) << why;
  EXPECT_NE(why.find("not listed"), std::string::npos) << why;

  // A bare base is not one of the listed types either.
  std::vector<std::unique_ptr<Behaviour>> bare;
  bare.push_back(std::make_unique<Behaviour>());
  EXPECT_FALSE(ForEachElement(bare, [](TypeId, auto&) {}, &why));

  std::vector<Behaviour*> with_null = {nullptr};
  EXPECT_FALSE(ForEachElement(with_null, [](TypeId, auto&) {}, &why));
  EXPECT_NE(why.find("element 0"), std::string::npos) << why;
  EXPECT_NE(why.find("null"), std::string::npos) << why;
}

TEST(PolymorphicList, AnUnknownIdentifierIsRefusedAndAppendsNothing) {
  std::vector<std::unique_ptr<Behaviour>> list;
  std::string why;
  bool filled = false;
  EXPECT_FALSE(AppendElement(
      list, detail::IdOf<Grow>() ^ 1, [&filled](auto&) { filled = true; },
      &why));
  EXPECT_TRUE(list.empty());
  EXPECT_FALSE(filled);
  EXPECT_NE(why.find("poly_test::Behaviour"), std::string::npos) << why;
}

}  // namespace
}  // namespace abmio
