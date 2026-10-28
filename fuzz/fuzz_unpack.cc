// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdint>

#include "abmio/exchange.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static abmio::Arena arena;  // reused across inputs, as a receiving rank would
  abmio::Unpack(reinterpret_cast<const char*>(data), size, arena);
  return 0;
}
