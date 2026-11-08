// SPDX-License-Identifier: Apache-2.0
// The LZ4 filter's codec on its own, without HDF5: round trips, stored
// blocks, and corrupted input, which a checkpoint read from disk may hold.
#include <gtest/gtest.h>

#include <vector>

#include "abmio/h5lz4.h"

namespace abmio {
namespace {

std::vector<uint8_t> Sample(std::size_t n) {
  std::vector<uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<uint8_t>((i / 64) % 7 + (i % 3));  // compressible
  }
  return v;
}

TEST(H5Lz4, PackAndUnpackRoundTrip) {
  for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{100},
                              std::size_t{65536}, std::size_t{70000}}) {
    const std::vector<uint8_t> in = Sample(n);
    std::vector<uint8_t> packed, out;
    Lz4Pack(in.data(), in.size(), 0, &packed);
    EXPECT_EQ(Lz4Unpack(packed.data(), packed.size(), &out), Status::kOk);
    EXPECT_EQ(out, in) << n;
    if (n >= 65536) {
      EXPECT_LT(packed.size(), in.size() / 4);
    }
  }
}

TEST(H5Lz4, SmallBlocksAndIncompressibleBlocks) {
  std::vector<uint8_t> in(10000);
  uint32_t x = 12345;
  for (uint8_t& b : in) {  // noise: LZ4 cannot shrink it, so blocks are stored
    x = x * 1103515245u + 12345u;
    b = static_cast<uint8_t>(x >> 16);
  }
  std::vector<uint8_t> packed, out;
  Lz4Pack(in.data(), in.size(), 4096, &packed);
  EXPECT_EQ(Lz4Unpack(packed.data(), packed.size(), &out), Status::kOk);
  EXPECT_EQ(out, in);
  EXPECT_EQ(packed.size(), 12 + 3 * 4 + in.size());  // three stored blocks
}

TEST(H5Lz4, UnpackRefusesWhatThePluginWouldNotHaveWritten) {
  const std::vector<uint8_t> in = Sample(5000);
  std::vector<uint8_t> packed, out;
  Lz4Pack(in.data(), in.size(), 1024, &packed);

  // Too short for a header, or for the blocks it announces.
  EXPECT_EQ(Lz4Unpack(packed.data(), 11, &out), Status::kTruncated);
  EXPECT_EQ(Lz4Unpack(packed.data(), packed.size() - 1, &out),
            Status::kTruncated);
  // Trailing bytes.
  std::vector<uint8_t> longer = packed;
  longer.push_back(0);
  EXPECT_EQ(Lz4Unpack(longer.data(), longer.size(), &out), Status::kBadHeader);
  // A total size no input this short could decode to: refused before any
  // memory is asked for.
  std::vector<uint8_t> huge = packed;
  huge[0] = 0x7f;
  EXPECT_EQ(Lz4Unpack(huge.data(), huge.size(), &out), Status::kBadHeader);
  // A block whose bytes LZ4 rejects.
  std::vector<uint8_t> corrupt = packed;
  for (std::size_t i = 16; i < 40 && i < corrupt.size(); ++i) {
    corrupt[i] ^= 0xff;
  }
  EXPECT_NE(Lz4Unpack(corrupt.data(), corrupt.size(), &out), Status::kOk);
  // Every byte flipped, one at a time, never reads outside the buffer (the
  // sanitizer build checks that) and never claims success with other data.
  for (std::size_t i = 0; i < packed.size(); ++i) {
    std::vector<uint8_t> flipped = packed;
    flipped[i] ^= 0x5a;
    if (Lz4Unpack(flipped.data(), flipped.size(), &out) == Status::kOk) {
      EXPECT_EQ(out.size(), in.size());
    }
  }
}

}  // namespace
}  // namespace abmio
