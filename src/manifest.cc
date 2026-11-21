// SPDX-License-Identifier: Apache-2.0
#include "abmio/manifest.h"

#include <set>

namespace abmio {
namespace {

Status Fail(std::string* why, const std::string& message) {
  if (why != nullptr) {
    *why = message;
  }
  return Status::kCorrupt;
}

bool NameCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// A parser for exactly the JSON these files use: objects, arrays, strings
// without escapes and unsigned integers.
class Parser {
 public:
  Parser(const char* data, std::size_t size) : p_(data), end_(data + size) {}

  bool Peek(char c) {
    Space();
    return p_ < end_ && *p_ == c;
  }
  bool Take(char c) {
    if (!Peek(c)) {
      return false;
    }
    ++p_;
    return true;
  }
  bool AtEnd() {
    Space();
    return p_ == end_;
  }
  // A string of printable ASCII other than '"' and '\\'.
  bool String(std::string* out) {
    if (!Take('"')) {
      return false;
    }
    out->clear();
    while (p_ < end_ && *p_ != '"') {
      const char c = *p_++;
      if (c < 0x20 || c > 0x7e || c == '\\' || out->size() >= 1024) {
        return false;
      }
      out->push_back(c);
    }
    if (p_ == end_) {
      return false;
    }
    ++p_;
    return true;
  }
  // Decimal digits, no sign, no leading zero, no fraction, at most 2^64 - 1.
  bool Unsigned(uint64_t* out) {
    Space();
    if (p_ == end_ || *p_ < '0' || *p_ > '9') {
      return false;
    }
    if (*p_ == '0' && p_ + 1 < end_ && p_[1] >= '0' && p_[1] <= '9') {
      return false;
    }
    uint64_t v = 0;
    while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
      const uint64_t digit = static_cast<uint64_t>(*p_ - '0');
      if (v > (UINT64_MAX - digit) / 10) {
        return false;
      }
      v = v * 10 + digit;
      ++p_;
    }
    if (p_ < end_ && (*p_ == '.' || *p_ == 'e' || *p_ == 'E')) {
      return false;
    }
    *out = v;
    return true;
  }

 private:
  void Space() {
    while (p_ < end_ &&
           (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) {
      ++p_;
    }
  }
  const char* p_;
  const char* end_;
};

// Calls key(name) for each key of an object; key reads the value. Repeated
// keys are refused.
template <class Key>
bool Object(Parser& in, Key&& key) {
  if (!in.Take('{')) {
    return false;
  }
  std::set<std::string> seen;
  if (in.Take('}')) {
    return true;
  }
  do {
    std::string name;
    if (!in.String(&name) || !seen.insert(name).second || !in.Take(':') ||
        !key(name)) {
      return false;
    }
  } while (in.Take(','));
  return in.Take('}');
}

bool Version(Parser& in, uint32_t supported, const char* what, std::string* why,
             bool* newer) {
  uint64_t v = 0;
  if (!in.Unsigned(&v) || v == 0) {
    return false;
  }
  if (v > supported) {
    *newer = true;
    Fail(why, std::string("the ") + what + " is of format " +
                  std::to_string(v) + ", newer than the " +
                  std::to_string(supported) + " this abm-io reads");
    return false;
  }
  return true;
}

}  // namespace

bool IsCheckpointName(const std::string& name) {
  if (name.empty() || name.size() > 128 || name[0] == '.' || name == "latest") {
    return false;
  }
  if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
    return false;
  }
  for (char c : name) {
    if (!NameCharacter(c)) {
      return false;
    }
  }
  return true;
}

std::string ToJson(const Manifest& manifest) {
  std::string s = "{\"abmio_manifest\": " + std::to_string(kManifestFormat) +
                  ", \"checkpoint\": \"" + manifest.checkpoint +
                  "\", \"parts\": [";
  for (std::size_t i = 0; i < manifest.parts.size(); ++i) {
    s += i == 0 ? "\n  " : ",\n  ";
    s += "{\"name\": \"" + manifest.parts[i].name +
         "\", \"bytes\": " + std::to_string(manifest.parts[i].bytes) + "}";
  }
  s += manifest.parts.empty() ? "]}\n" : "\n]}\n";
  return s;
}

Status ParseManifest(const char* data, std::size_t size, Manifest* out,
                     std::string* why) {
  const std::string bad = "the manifest is malformed";
  if (size > kMaxManifestBytes) {
    return Fail(why, "the manifest is larger than abm-io accepts");
  }
  Manifest m;
  Parser in(data, size);
  bool version = false, newer = false, has_parts = false;
  std::set<std::string> names;
  const bool ok = Object(in, [&](const std::string& key) {
    if (key == "abmio_manifest") {
      version = Version(in, kManifestFormat, "manifest", why, &newer);
      return version;
    }
    if (key == "checkpoint") {
      return in.String(&m.checkpoint) && IsCheckpointName(m.checkpoint);
    }
    if (key == "parts") {
      has_parts = true;
      if (!in.Take('[')) {
        return false;
      }
      if (in.Take(']')) {
        return true;
      }
      do {
        ManifestPart part;
        bool has_name = false, has_bytes = false;
        const bool part_ok = Object(in, [&](const std::string& k) {
          if (k == "name") {
            has_name = true;
            return in.String(&part.name) && IsCheckpointName(part.name);
          }
          if (k == "bytes") {
            has_bytes = true;
            return in.Unsigned(&part.bytes);
          }
          return false;
        });
        if (!part_ok || !has_name || !has_bytes ||
            !names.insert(part.name).second) {
          return false;
        }
        m.parts.push_back(std::move(part));
      } while (in.Take(','));
      return in.Take(']');
    }
    return false;
  });
  if (newer) {
    return Status::kBadHeader;
  }
  if (!ok || !in.AtEnd() || !version || m.checkpoint.empty() || !has_parts) {
    return Fail(why, bad);
  }
  *out = std::move(m);
  return Status::kOk;
}

std::string PointerJson(const std::string& checkpoint) {
  return "{\"abmio_pointer\": " + std::to_string(kPointerFormat) +
         ", \"checkpoint\": \"" + checkpoint + "\"}\n";
}

Status ParsePointer(const char* data, std::size_t size, std::string* checkpoint,
                    std::string* why) {
  if (size > kMaxManifestBytes) {
    return Fail(why, "the pointer is larger than abm-io accepts");
  }
  Parser in(data, size);
  bool version = false, newer = false;
  std::string name;
  const bool ok = Object(in, [&](const std::string& key) {
    if (key == "abmio_pointer") {
      version = Version(in, kPointerFormat, "pointer", why, &newer);
      return version;
    }
    if (key == "checkpoint") {
      return in.String(&name) && IsCheckpointName(name);
    }
    return false;
  });
  if (newer) {
    return Status::kBadHeader;
  }
  if (!ok || !in.AtEnd() || !version || name.empty()) {
    return Fail(why, "the pointer is malformed");
  }
  *checkpoint = name;
  return Status::kOk;
}

}  // namespace abmio
