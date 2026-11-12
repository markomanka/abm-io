// SPDX-License-Identifier: Apache-2.0
#include "abmio/schema.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

// A small model exercising every kind of field the visitor protocol describes.
namespace demo {
struct Behavior {
  virtual ~Behavior() = default;
};
struct Growth : Behavior {
  double rate = 0;
};
struct Phenotype {
  float volume = 0;
  int32_t phase = 0;
};
struct Agent {
  uint64_t uid = 0;
  uint32_t box = 0;
};
struct Cell : Agent {
  double position[3] = {0, 0, 0};
  std::array<float, 3> force{};
  double diameter = 10;
  uint64_t mother = 0;
  std::vector<uint64_t> neighbours;
  std::vector<double> concentrations;
  std::string label;
  Phenotype phenotype;
  std::vector<std::unique_ptr<Behavior>> behaviors;
};
struct Scalars {
  bool alive = true;
  char code = 'a';
  int8_t i8 = 0;
  uint8_t u8 = 0;
  int16_t i16 = 0;
  uint16_t u16 = 0;
  int64_t i64 = 0;
  std::vector<Behavior*> plain_list;
};
struct Adhesion {
  double strength = 0;
};
struct BadReference {
  double x = 0;
};
}  // namespace demo

namespace abmio {
template <>
struct TypeTraits<demo::Agent> {
  static constexpr const char* kName = "demo::Agent";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("uid").Reference(), &demo::Agent::uid);
    v(Field("box").Roles(Role::kNone), &demo::Agent::box);
  }
};
template <>
struct TypeTraits<demo::Phenotype> {
  static constexpr const char* kName = "demo::Phenotype";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("volume").Unit("um3"), &demo::Phenotype::volume);
    v(Field("phase"), &demo::Phenotype::phase);
  }
};
template <>
struct TypeTraits<demo::Behavior> {
  static constexpr const char* kName = "demo::Behavior";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V&) {}
};
template <>
struct TypeTraits<demo::Growth> {
  static constexpr const char* kName = "demo::Growth";
  static constexpr uint32_t kVersion = 3;
  static constexpr TypeId kId = 0x1234;  // as if kept from an earlier name
  using Base = demo::Behavior;
  template <class V>
  static void Visit(V& v) {
    v(Field("rate").Unit("1/h"), &demo::Growth::rate);
  }
};
template <>
struct TypeTraits<demo::Cell> {
  static constexpr const char* kName = "demo::Cell";
  static constexpr uint32_t kVersion = 2;
  static constexpr const char* kTerm = "CL:0000000";
  using Base = demo::Agent;
  template <class V>
  static void Visit(V& v) {
    v(Field("position").Unit("um"), &demo::Cell::position);
    v(Field("force").Roles(Role::kExport), &demo::Cell::force);
    v(Field("diameter").Unit("um").StoredAs(ElementType::kFloat32),
      &demo::Cell::diameter);
    v(Field("mother").Reference().Term("RO:0002202"), &demo::Cell::mother);
    v(Field("neighbours").Reference().Roles(Role::kNone),
      &demo::Cell::neighbours);
    v(Field("concentrations")
          .Unit("uM")
          .Roles(Role::kCheckpoint | Role::kExport),
      &demo::Cell::concentrations);
    v(Field("label"), &demo::Cell::label);
    v(Field("phenotype"), &demo::Cell::phenotype);
    v(Field("behaviors"), &demo::Cell::behaviors);
  }
};
template <>
struct TypeTraits<demo::Scalars> {
  static constexpr const char* kName = "demo::Scalars";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("alive"), &demo::Scalars::alive);
    v(Field("code"), &demo::Scalars::code);
    v(Field("i8"), &demo::Scalars::i8);
    v(Field("u8"), &demo::Scalars::u8);
    v(Field("i16"), &demo::Scalars::i16);
    v(Field("u16"), &demo::Scalars::u16);
    v(Field("i64").StoredAs(ElementType::kInt32), &demo::Scalars::i64);
    v(Field("plain_list"), &demo::Scalars::plain_list);
  }
};
template <>
struct TypeTraits<demo::Adhesion> {
  static constexpr const char* kName = "demo::Adhesion";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("strength").Unit("nN"), &demo::Adhesion::strength);
  }
};
template <>
struct TypeTraits<demo::BadReference> {
  static constexpr const char* kName = "demo::BadReference";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("x").Reference(), &demo::BadReference::x);
  }
};
}  // namespace abmio

namespace abmio {
namespace {

static_assert(TypeIdOf("") == 0xcbf29ce484222325ULL,
              "FNV-1a 64 of the empty string");
static_assert(TypeIdOf("a") == 0xaf63dc4c8601ec8cULL, "FNV-1a 64 test vector");
static_assert(TypeIdOf("foobar") == 0x85944171f73967e8ULL,
              "FNV-1a 64 test vector");

Schema DemoSchema() {
  Schema s;
  s.types = {Describe<demo::Agent>(),     Describe<demo::Cell>(),
             Describe<demo::Phenotype>(), Describe<demo::Behavior>(),
             Describe<demo::Growth>(),    Describe<demo::Scalars>()};
  s.relations = {{"adhesion", "RO:0002170", DescribeFields<demo::Adhesion>()}};
  return s;
}

TypeDescription& TypeNamed(Schema& s, const std::string& name) {
  for (auto& t : s.types) {
    if (t.qualified_name == name) {
      return t;
    }
  }
  ADD_FAILURE() << "no type " << name;
  return s.types.front();
}

FieldDescription& FieldNamed(TypeDescription& t, const std::string& name) {
  for (auto& f : t.fields) {
    if (f.name == name) {
      return f;
    }
  }
  ADD_FAILURE() << "no field " << name;
  return t.fields.front();
}

void ExpectInvalid(const Schema& s, const std::string& expected) {
  std::string why;
  EXPECT_EQ(Validate(s, &why), Status::kBadSchema);
  EXPECT_NE(why.find(expected), std::string::npos) << "message was: " << why;
  EXPECT_EQ(Validate(s), Status::kBadSchema);  // the message is optional
}

TEST(Schema, AgentIdsAreUniqueAcrossProcessesAndRestarts) {
  std::set<AgentId> ids;
  AgentIdAllocator rank0(0, 2), rank1(1, 2);
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(ids.insert(rank0.Next()).second);
    EXPECT_TRUE(ids.insert(rank1.Next()).second);
  }
  EXPECT_EQ(ids, (std::set<AgentId>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(ids.count(kNoAgent), 0u);
  // A restart on three processes continues after the largest stored identifier.
  const AgentId first = *ids.rbegin() + 1;
  for (uint32_t rank = 0; rank < 3; ++rank) {
    AgentIdAllocator a(rank, 3, first);
    EXPECT_EQ(a.Peek(), first + rank);
    for (int i = 0; i < 4; ++i) {
      EXPECT_TRUE(ids.insert(a.Next()).second);
    }
  }
  EXPECT_EQ(ids.size(), 18u);
}

TEST(Schema, Roles) {
  EXPECT_TRUE(HasRole(Role::kAll, Role::kExchange));
  EXPECT_TRUE(HasRole(Role::kCheckpoint | Role::kExport, Role::kExport));
  EXPECT_FALSE(HasRole(Role::kCheckpoint | Role::kExport, Role::kExchange));
  EXPECT_FALSE(HasRole(Role::kCheckpoint, Role::kCheckpoint | Role::kExchange));
  EXPECT_FALSE(HasRole(Role::kAll, Role::kNone));
}

TEST(Schema, ElementTypesAndNames) {
  const std::vector<std::pair<ElementType, std::size_t>> sizes = {
      {ElementType::kNone, 0},    {ElementType::kBool, 1},
      {ElementType::kChar, 1},    {ElementType::kInt8, 1},
      {ElementType::kUInt8, 1},   {ElementType::kInt16, 2},
      {ElementType::kUInt16, 2},  {ElementType::kInt32, 4},
      {ElementType::kUInt32, 4},  {ElementType::kInt64, 8},
      {ElementType::kUInt64, 8},  {ElementType::kFloat32, 4},
      {ElementType::kFloat64, 8}, {ElementType::kAgentId, 8}};
  std::set<std::string> names;
  for (const auto& [type, size] : sizes) {
    EXPECT_EQ(SizeOf(type), size) << ToString(type);
    EXPECT_TRUE(names.insert(ToString(type)).second);
  }
  EXPECT_STREQ(ToString(ElementType::kAgentId), "agent id");
  EXPECT_EQ(SizeOf(static_cast<ElementType>(250)), 0u);
  EXPECT_STREQ(ToString(static_cast<ElementType>(250)), "unknown");
  for (Kind k : {Kind::kScalar, Kind::kArray, Kind::kVector, Kind::kString,
                 Kind::kRecord, Kind::kPolymorphicList}) {
    EXPECT_TRUE(names.insert(ToString(k)).second) << ToString(k);
  }
  EXPECT_STREQ(ToString(static_cast<Kind>(250)), "unknown");
  EXPECT_STREQ(ToString(Status::kBadSchema), "bad schema");
}

TEST(Schema, DescribeDerivesTheDescriptionFromTheVisitFunction) {
  const TypeDescription cell = Describe<demo::Cell>();
  EXPECT_EQ(cell.qualified_name, "demo::Cell");
  EXPECT_EQ(cell.id, TypeIdOf("demo::Cell"));
  EXPECT_EQ(cell.base, TypeIdOf("demo::Agent"));
  EXPECT_EQ(cell.version, 2u);
  EXPECT_EQ(cell.term, "CL:0000000");
  ASSERT_EQ(cell.fields.size(),
            9u);  // own fields only; the base's are described with it

  const auto& position = cell.fields[0];
  EXPECT_EQ(position.name, "position");
  EXPECT_EQ(position.kind, Kind::kArray);
  EXPECT_EQ(position.element, ElementType::kFloat64);
  EXPECT_EQ(position.count, 3u);
  EXPECT_EQ(position.unit, "um");
  EXPECT_EQ(position.roles, Role::kAll);

  const auto& force = cell.fields[1];
  EXPECT_EQ(force.kind, Kind::kArray);
  EXPECT_EQ(force.element, ElementType::kFloat32);
  EXPECT_EQ(force.count, 3u);
  EXPECT_EQ(force.roles, Role::kExport);

  const auto& diameter = cell.fields[2];
  EXPECT_EQ(diameter.kind, Kind::kScalar);
  EXPECT_EQ(diameter.element, ElementType::kFloat64);
  EXPECT_EQ(diameter.stored, ElementType::kFloat32);

  const auto& mother = cell.fields[3];
  EXPECT_EQ(mother.element, ElementType::kAgentId);
  EXPECT_EQ(mother.term, "RO:0002202");

  const auto& neighbours = cell.fields[4];
  EXPECT_EQ(neighbours.kind, Kind::kVector);
  EXPECT_EQ(neighbours.element, ElementType::kAgentId);
  EXPECT_EQ(neighbours.roles, Role::kNone);

  const auto& concentrations = cell.fields[5];
  EXPECT_EQ(concentrations.kind, Kind::kVector);
  EXPECT_EQ(concentrations.count, 0u);
  EXPECT_EQ(concentrations.roles, Role::kCheckpoint | Role::kExport);

  const auto& label = cell.fields[6];
  EXPECT_EQ(label.kind, Kind::kString);
  EXPECT_EQ(label.element, ElementType::kChar);

  const auto& phenotype = cell.fields[7];
  EXPECT_EQ(phenotype.kind, Kind::kRecord);
  EXPECT_EQ(phenotype.element, ElementType::kNone);
  EXPECT_EQ(phenotype.record_type, TypeIdOf("demo::Phenotype"));

  const auto& behaviors = cell.fields[8];
  EXPECT_EQ(behaviors.kind, Kind::kPolymorphicList);
  EXPECT_EQ(behaviors.record_type, TypeIdOf("demo::Behavior"));
}

TEST(Schema, ScalarsExplicitIdsAndRelations) {
  const TypeDescription scalars = Describe<demo::Scalars>();
  const std::vector<ElementType> expected = {
      ElementType::kBool,  ElementType::kChar,  ElementType::kInt8,
      ElementType::kUInt8, ElementType::kInt16, ElementType::kUInt16,
      ElementType::kInt64};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(scalars.fields[i].kind, Kind::kScalar);
    EXPECT_EQ(scalars.fields[i].element, expected[i]) << scalars.fields[i].name;
  }
  EXPECT_EQ(scalars.fields[7].kind,
            Kind::kPolymorphicList);  // plain pointers work too

  const TypeDescription growth = Describe<demo::Growth>();
  EXPECT_EQ(growth.id, 0x1234u);
  EXPECT_EQ(growth.base, TypeIdOf("demo::Behavior"));
  EXPECT_EQ(Describe<demo::Agent>().term, "");

  const Schema s = DemoSchema();
  std::string why;
  EXPECT_EQ(Validate(s, &why), Status::kOk) << why;
  ASSERT_NE(s.Find(0x1234), nullptr);
  EXPECT_EQ(s.Find(0x1234)->qualified_name, "demo::Growth");
  EXPECT_EQ(s.Find(42), nullptr);
  EXPECT_EQ(s.relations[0].attributes.size(), 1u);
  EXPECT_EQ(s.relations[0].attributes[0].unit, "nN");
}

TEST(Schema, InvalidTypes) {
  {
    Schema s = DemoSchema();
    s.types[0].qualified_name = "";
    ExpectInvalid(s, "a type has no name");
  }
  {
    Schema s = DemoSchema();
    s.types[0].qualified_name = "bad\x01name";
    ExpectInvalid(s, "not printable");
  }
  {
    Schema s = DemoSchema();
    TypeNamed(s, "demo::Scalars").id = kNoType;
    ExpectInvalid(s, "means \"no type\"");
  }
  {
    Schema s = DemoSchema();
    s.types.push_back(Describe<demo::Phenotype>());
    ExpectInvalid(s, "described twice");
  }
  {
    Schema s = DemoSchema();
    TypeDescription copy = Describe<demo::Phenotype>();
    copy.qualified_name = "demo::Renamed";  // same identifier, different name
    s.types.push_back(copy);
    ExpectInvalid(s, "share a type identifier");
  }
  {
    Schema s = DemoSchema();
    TypeNamed(s, "demo::Cell").base = TypeIdOf("demo::Missing");
    ExpectInvalid(s, "its base type is not in the schema");
  }
  {
    Schema s = DemoSchema();
    TypeNamed(s, "demo::Agent").base = TypeIdOf("demo::Cell");
    ExpectInvalid(s, "form a cycle");
  }
  {
    Schema s = DemoSchema();
    FieldDescription uid = TypeNamed(s, "demo::Agent").fields[0];
    TypeNamed(s, "demo::Cell").fields.push_back(uid);  // already in the base
    ExpectInvalid(s, "field uid appears twice");
  }
}

TEST(Schema, InvalidFields) {
  const auto with = [](void (*change)(FieldDescription&),
                       const std::string& field) {
    Schema s = DemoSchema();
    change(FieldNamed(TypeNamed(s, "demo::Cell"), field));
    return s;
  };
  ExpectInvalid(with([](FieldDescription& f) { f.name = ""; }, "label"),
                "a field has no name");
  ExpectInvalid(with([](FieldDescription& f) { f.unit = "u\tm"; }, "position"),
                "printable");
  ExpectInvalid(with([](FieldDescription& f) { f.count = 2; }, "diameter"),
                "exactly one element");
  ExpectInvalid(with([](FieldDescription& f) { f.count = 0; }, "position"),
                "at least one");
  ExpectInvalid(
      with([](FieldDescription& f) { f.count = 3; }, "concentrations"),
      "no fixed count");
  ExpectInvalid(
      with([](FieldDescription& f) { f.element = ElementType::kInt32; },
           "label"),
      "holds characters");
  ExpectInvalid(
      with([](FieldDescription& f) { f.element = ElementType::kFloat64; },
           "phenotype"),
      "no element type and no count");
  ExpectInvalid(
      with([](FieldDescription& f) { f.record_type = 7; }, "phenotype"),
      "record type is not in the schema");
  ExpectInvalid(
      with([](FieldDescription& f) { f.record_type = 7; }, "behaviors"),
      "record type is not in the schema");
  ExpectInvalid(
      with([](FieldDescription& f) { f.stored = ElementType::kInt32; },
           "diameter"),
      "cannot be stored as int32");
  ExpectInvalid(
      with([](FieldDescription& f) { f.stored = ElementType::kFloat64; },
           "force"),
      "cannot be stored as float64");
  ExpectInvalid(
      with([](FieldDescription& f) { f.stored = ElementType::kInt16; },
           "mother"),
      "cannot be stored as int16");
  {
    Schema s = DemoSchema();
    FieldNamed(TypeNamed(s, "demo::Scalars"), "u16").stored =
        ElementType::kInt8;
    ExpectInvalid(s, "cannot be stored as int8");  // signedness must match
  }
  {
    Schema s = DemoSchema();
    s.types.push_back(
        Describe<demo::BadReference>());  // Reference() on a double
    ExpectInvalid(s, "a reference must hold 64-bit unsigned integers");
  }
}

TEST(Schema, InvalidRelations) {
  {
    Schema s = DemoSchema();
    s.relations[0].name = "";
    ExpectInvalid(s, "a relation has no name");
  }
  {
    Schema s = DemoSchema();
    s.relations.push_back(s.relations[0]);
    ExpectInvalid(s, "relation adhesion: described twice");
  }
  {
    Schema s = DemoSchema();
    s.relations[0].attributes.push_back(s.relations[0].attributes[0]);
    ExpectInvalid(s, "attribute strength appears twice");
  }
  {
    Schema s = DemoSchema();
    s.relations[0].attributes[0].count = 0;
    ExpectInvalid(
        s, "relation adhesion.strength: a scalar has exactly one element");
  }
}

TEST(SchemaJson, TheSameSchemaGivesTheSameTextAndNothingBreaksIt) {
  Schema s;
  TypeDescription t;
  t.qualified_name = "ns::Odd\"Name\\";
  t.id = TypeIdOf("ns::Odd");
  t.version = 2;
  FieldDescription f;
  f.name = "note";
  f.kind = Kind::kString;
  f.unit = "a\tb\nc\x01";
  f.roles = Role::kCheckpoint | Role::kExport;
  t.fields.push_back(f);
  s.types.push_back(t);
  RelationDescription r;
  r.name = "adhesion";
  s.relations.push_back(r);

  const std::string json = ToJson(s);
  EXPECT_EQ(json, ToJson(s));
  // Quotes, backslashes and control characters are escaped, so the text stays
  // valid JSON whatever a name or a unit contains.
  EXPECT_NE(json.find("\"name\":\"ns::Odd\\\"Name\\\\\""), std::string::npos)
      << json;
  EXPECT_NE(json.find("\"unit\":\"a\\tb\\nc\\u0001\""), std::string::npos)
      << json;
  // No base: null. Identifiers: hexadecimal strings.
  EXPECT_NE(json.find("\"base\":null"), std::string::npos) << json;
  EXPECT_NE(json.find("\"id\":\"0x"), std::string::npos) << json;
  EXPECT_NE(json.find("\"roles\":[\"checkpoint\",\"export\"]"),
            std::string::npos)
      << json;
  EXPECT_NE(json.find("\"relations\":[{\"name\":\"adhesion\""),
            std::string::npos)
      << json;
}

}  // namespace
}  // namespace abmio
