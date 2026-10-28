// SPDX-License-Identifier: Apache-2.0
#include "abmio/status.h"

namespace abmio {
const char* ToString(Status s) {
  switch (s) {
    case Status::kOk:
      return "ok";
    case Status::kTruncated:
      return "truncated";
    case Status::kBadHeader:
      return "bad header";
    case Status::kBadIndex:
      return "bad index";
    case Status::kSizeMismatch:
      return "size mismatch";
    case Status::kCorrupt:
      return "corrupt";
  }
  return "unknown";
}
}  // namespace abmio
