// SPDX-License-Identifier: Apache-2.0
// The two halves of the LZ4 filter (abmio/h5lz4.h), kept apart from the HDF5
// callback so that they build, test and fuzz without HDF5.
#include <lz4.h>

#include <cstring>
#include <limits>

#include "abmio/h5lz4.h"

namespace abmio {
namespace {

void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

void PutBe64(uint8_t* p, uint64_t v) {
  PutBe32(p, static_cast<uint32_t>(v >> 32));
  PutBe32(p + 4, static_cast<uint32_t>(v));
}

uint32_t GetBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t GetBe64(const uint8_t* p) {
  return (static_cast<uint64_t>(GetBe32(p)) << 32) | GetBe32(p + 4);
}

}  // namespace

void Lz4Pack(const uint8_t* in, std::size_t n, uint32_t block,
             std::vector<uint8_t>* out) {
  if (block == 0) {
    block = kLz4DefaultBlock;
  }
  if (block > n) {
    block = static_cast<uint32_t>(n);  // the plugin shrinks the block to fit
  }
  const std::size_t blocks = n == 0 ? 1 : (n + block - 1) / block;
  // LZ4_compressBound is defined for inputs up to LZ4_MAX_INPUT_SIZE, which the
  // default block respects; a block larger than that is stored as it is.
  const std::size_t bound =
      block <= static_cast<uint32_t>(LZ4_MAX_INPUT_SIZE)
          ? static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(block)))
          : block;
  out->resize(12 + blocks * (4 + bound));
  uint8_t* op = out->data();
  PutBe64(op, n);
  PutBe32(op + 8, block);
  op += 12;
  for (std::size_t offset = 0; offset < n; offset += block) {
    const std::size_t this_block = n - offset < block ? n - offset : block;
    int packed = 0;
    if (this_block <= static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
      packed = LZ4_compress_default(reinterpret_cast<const char*>(in + offset),
                                    reinterpret_cast<char*>(op + 4),
                                    static_cast<int>(this_block),
                                    static_cast<int>(bound));
    }
    if (packed <= 0 || static_cast<std::size_t>(packed) >= this_block) {
      std::memcpy(op + 4, in + offset, this_block);  // stored, not compressed
      packed = static_cast<int>(this_block);
    }
    PutBe32(op, static_cast<uint32_t>(packed));
    op += 4 + packed;
  }
  out->resize(static_cast<std::size_t>(op - out->data()));
}

Status Lz4Unpack(const uint8_t* in, std::size_t n, std::vector<uint8_t>* out) {
  if (n < 12) {
    return Status::kTruncated;
  }
  const uint64_t total = GetBe64(in);
  uint64_t block = GetBe32(in + 8);
  // LZ4 expands a byte to at most 255, so a header claiming more than that
  // of its input is wrong, and is refused before any memory is asked for.
  if (total > static_cast<uint64_t>(n) * 255 ||
      total > std::numeric_limits<std::size_t>::max() / 2) {
    return Status::kBadHeader;
  }
  if (block > total) {
    block = total;  // as the plugin reads it
  }
  if (total > 0 && block == 0) {
    return Status::kBadHeader;
  }
  const uint8_t* ip = in + 12;
  const uint8_t* end = in + n;
  out->resize(static_cast<std::size_t>(total));
  for (uint64_t offset = 0; offset < total; offset += block) {
    const uint64_t this_block = total - offset < block ? total - offset : block;
    if (static_cast<std::size_t>(end - ip) < 4) {
      return Status::kTruncated;
    }
    const uint32_t packed = GetBe32(ip);
    ip += 4;
    if (static_cast<std::size_t>(end - ip) < packed) {
      return Status::kTruncated;
    }
    uint8_t* op = out->data() + offset;
    if (packed == this_block) {
      std::memcpy(op, ip, packed);  // stored block
    } else {
      if (packed > this_block ||
          this_block > static_cast<uint64_t>(LZ4_MAX_INPUT_SIZE)) {
        return Status::kBadHeader;
      }
      const int got = LZ4_decompress_safe(
          reinterpret_cast<const char*>(ip), reinterpret_cast<char*>(op),
          static_cast<int>(packed), static_cast<int>(this_block));
      if (got < 0 || static_cast<uint64_t>(got) != this_block) {
        return Status::kCorrupt;
      }
    }
    ip += packed;
  }
  if (ip != end) {
    return Status::kBadHeader;  // trailing bytes: not a frame the plugin wrote
  }
  return Status::kOk;
}

}  // namespace abmio
