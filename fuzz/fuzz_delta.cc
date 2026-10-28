// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "abmio/codec.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<char> prev, out;
  if (size >=
      16) {  // offer a reference of the claimed size when it is small enough
    uint64_t n;
    std::memcpy(&n, data + 8, 8);
    if (n <= (uint64_t{1} << 20))
      prev.assign(n, 0x5a);
  }
  abmio::DecodeDelta(reinterpret_cast<const char*>(data), size,
                     prev.empty() ? nullptr : prev.data(), prev.size(), out);
  return 0;
}
