// SPDX-License-Identifier: Apache-2.0
#include "abmio/schema.h"

#include <set>
#include <string>
#include <utility>

namespace abmio {

std::size_t SizeOf(ElementType t) {
  switch (t) {
    case ElementType::kNone:
      return 0;
    case ElementType::kBool:
    case ElementType::kChar:
    case ElementType::kInt8:
    case ElementType::kUInt8:
      return 1;
    case ElementType::kInt16:
    case ElementType::kUInt16:
      return 2;
    case ElementType::kInt32:
    case ElementType::kUInt32:
    case ElementType::kFloat32:
      return 4;
    case ElementType::kInt64:
    case ElementType::kUInt64:
    case ElementType::kFloat64:
    case ElementType::kAgentId:
      return 8;
  }
  return 0;
}

const char* ToString(ElementType t) {
  switch (t) {
    case ElementType::kNone:
      return "none";
    case ElementType::kBool:
      return "bool";
    case ElementType::kChar:
      return "char";
    case ElementType::kInt8:
      return "int8";
    case ElementType::kUInt8:
      return "uint8";
    case ElementType::kInt16:
      return "int16";
    case ElementType::kUInt16:
      return "uint16";
    case ElementType::kInt32:
      return "int32";
    case ElementType::kUInt32:
      return "uint32";
    case ElementType::kInt64:
      return "int64";
    case ElementType::kUInt64:
      return "uint64";
    case ElementType::kFloat32:
      return "float32";
    case ElementType::kFloat64:
      return "float64";
    case ElementType::kAgentId:
      return "agent id";
  }
  return "unknown";
}

const char* ToString(Kind k) {
  switch (k) {
    case Kind::kScalar:
      return "scalar";
    case Kind::kArray:
      return "array";
    case Kind::kVector:
      return "vector";
    case Kind::kString:
      return "string";
    case Kind::kRecord:
      return "record";
    case Kind::kPolymorphicList:
      return "polymorphic list";
  }
  return "unknown";
}

const TypeDescription* Schema::Find(TypeId id) const {
  for (const auto& t : types) {
    if (t.id == id) {
      return &t;
    }
  }
  return nullptr;
}

namespace {

Status Fail(std::string* why, std::string message) {
  if (why != nullptr) {
    *why = std::move(message);
  }
  return Status::kBadSchema;
}

bool IsFloat(ElementType t) {
  return t == ElementType::kFloat32 || t == ElementType::kFloat64;
}

bool IsSignedInteger(ElementType t) {
  return t == ElementType::kInt8 || t == ElementType::kInt16 ||
         t == ElementType::kInt32 || t == ElementType::kInt64;
}

bool IsUnsignedInteger(ElementType t) {
  return t == ElementType::kUInt8 || t == ElementType::kUInt16 ||
         t == ElementType::kUInt32 || t == ElementType::kUInt64;
}

// A narrower (or equal) element type of the same category and signedness.
bool CanStoreAs(ElementType element, ElementType stored) {
  const bool same_category =
      (IsFloat(element) && IsFloat(stored)) ||
      (IsSignedInteger(element) && IsSignedInteger(stored)) ||
      (IsUnsignedInteger(element) && IsUnsignedInteger(stored));
  return same_category && SizeOf(stored) <= SizeOf(element);
}

bool IsPrintable(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }
  return true;
}

Status CheckField(const FieldDescription& f, const Schema& schema,
                  const std::string& owner, std::string* why) {
  if (f.name.empty()) {
    return Fail(why, owner + ": a field has no name");
  }
  const std::string where = owner + "." + f.name;
  if (!IsPrintable(f.name) || !IsPrintable(f.unit) || !IsPrintable(f.term)) {
    return Fail(why,
                where + ": names, units and terms must be printable ASCII");
  }
  const bool has_elements =
      f.kind != Kind::kRecord && f.kind != Kind::kPolymorphicList;
  if (has_elements && f.element == ElementType::kNone) {
    return Fail(why, where +
                         ": no element type (a reference must hold 64-bit "
                         "unsigned integers)");
  }
  switch (f.kind) {
    case Kind::kScalar:
      if (f.count != 1) {
        return Fail(why, where + ": a scalar has exactly one element");
      }
      break;
    case Kind::kArray:
      if (f.count == 0) {
        return Fail(why, where + ": an array needs at least one element");
      }
      break;
    case Kind::kVector:
      if (f.count != 0) {
        return Fail(why, where + ": a vector has no fixed count");
      }
      break;
    case Kind::kString:
      if (f.count != 0 || f.element != ElementType::kChar) {
        return Fail(
            why, where + ": a string holds characters and has no fixed count");
      }
      break;
    case Kind::kRecord:
    case Kind::kPolymorphicList:
      if (f.element != ElementType::kNone || f.count != 0) {
        return Fail(
            why, where + ": a record or list has no element type and no count");
      }
      if (schema.Find(f.record_type) == nullptr) {
        return Fail(why, where + ": its record type is not in the schema");
      }
      break;
  }
  if (f.stored != ElementType::kNone && f.stored != f.element &&
      !CanStoreAs(f.element, f.stored)) {
    return Fail(why,
                where + ": cannot be stored as " + ToString(f.stored) +
                    ", which is not a narrower type of the same category as " +
                    ToString(f.element));
  }
  return Status::kOk;
}

}  // namespace

Status Validate(const Schema& schema, std::string* why) {
  std::set<std::string> type_names;
  for (const auto& t : schema.types) {
    if (t.qualified_name.empty() || !IsPrintable(t.qualified_name)) {
      return Fail(why,
                  "a type has no name, or one that is not printable ASCII");
    }
    if (t.id == kNoType) {
      return Fail(why,
                  t.qualified_name + ": type identifier 0 means \"no type\"");
    }
    if (!type_names.insert(t.qualified_name).second) {
      return Fail(why, t.qualified_name + ": described twice");
    }
    const TypeDescription* first = schema.Find(t.id);
    if (first != &t) {
      return Fail(
          why,
          t.qualified_name + " and " + first->qualified_name +
              " share a type identifier; give one of them an explicit kId");
    }
  }
  for (const auto& t : schema.types) {
    // Walk the base types: each must exist, and the chain must end.
    std::set<std::string> field_names;
    std::set<TypeId> seen;
    const TypeDescription* current = &t;
    while (current != nullptr) {
      if (!seen.insert(current->id).second) {
        return Fail(why, t.qualified_name + ": its base types form a cycle");
      }
      for (const auto& f : current->fields) {
        if (!field_names.insert(f.name).second) {
          return Fail(why, t.qualified_name + ": field " + f.name +
                               " appears twice in the type or its base types");
        }
      }
      if (current->base == kNoType) {
        break;
      }
      const TypeDescription* base = schema.Find(current->base);
      if (base == nullptr) {
        return Fail(why, current->qualified_name +
                             ": its base type is not in the schema");
      }
      current = base;
    }
    for (const auto& f : t.fields) {
      const Status s = CheckField(f, schema, t.qualified_name, why);
      if (s != Status::kOk) {
        return s;
      }
    }
  }
  std::set<std::string> relation_names;
  for (const auto& r : schema.relations) {
    if (r.name.empty() || !IsPrintable(r.name) || !IsPrintable(r.term)) {
      return Fail(why,
                  "a relation has no name, or a name or term that is not "
                  "printable ASCII");
    }
    if (!relation_names.insert(r.name).second) {
      return Fail(why, "relation " + r.name + ": described twice");
    }
    std::set<std::string> attribute_names;
    for (const auto& f : r.attributes) {
      if (!attribute_names.insert(f.name).second) {
        return Fail(why, "relation " + r.name + ": attribute " + f.name +
                             " appears twice");
      }
      const Status s = CheckField(f, schema, "relation " + r.name, why);
      if (s != Status::kOk) {
        return s;
      }
    }
  }
  return Status::kOk;
}

}  // namespace abmio
