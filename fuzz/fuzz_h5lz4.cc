// SPDX-License-Identifier: Apache-2.0
// The decoding half of the LZ4 filter on arbitrary input: a checkpoint read
// from disk is untrusted like a message, and HDF5 hands the filter whatever
// the chunk holds.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "abmio/h5lz4.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  abmio::Lz4Unpack(data, size, &out);
  return 0;
}
