// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace abmio {

enum class Status : uint8_t {
  kOk,
  kTruncated,     // buffer shorter than its headers claim
  kBadHeader,     // wrong magic, version, layout or inconsistent sizes
  kBadIndex,      // owner or slot index out of range
  kSizeMismatch,  // section sizes or reference buffer do not match
  kCorrupt,       // payload fails a consistency check
  kBadSchema      // a schema is inconsistent (see Validate in abmio/schema.h)
};

const char* ToString(Status s);

}  // namespace abmio
