// SPDX-License-Identifier: Apache-2.0
// Frame: "ABDZ" | flags u8 (bit0 shuffle, bit1 xor with previous) | 0 u8[3] |
// raw size u64 | LZ4 block
#include "abmio/codec.h"

#include <lz4.h>

#include <climits>
#include <cstring>
#include <stdexcept>

namespace abmio {
namespace {
constexpr char kMagic[4] = {'A', 'B', 'D', 'Z'};
constexpr uint64_t kHeader = 16;
constexpr uint8_t kShuffle = 1, kXor = 2;

void Shuffle(const char* in, char* out, uint64_t n) {
  const uint64_t m = n / 8;
  for (uint64_t i = 0; i < m; ++i)
    for (uint64_t b = 0; b < 8; ++b)
      out[b * m + i] = in[i * 8 + b];
  std::memcpy(out + 8 * m, in + 8 * m, n - 8 * m);
}
void Unshuffle(const char* in, char* out, uint64_t n) {
  const uint64_t m = n / 8;
  for (uint64_t i = 0; i < m; ++i)
    for (uint64_t b = 0; b < 8; ++b)
      out[i * 8 + b] = in[b * m + i];
  std::memcpy(out + 8 * m, in + 8 * m, n - 8 * m);
}
}  // namespace

std::vector<char> EncodeDelta(const char* cur, uint64_t n, const char* prev,
                              bool shuffle) {
  if (n > static_cast<uint64_t>(LZ4_MAX_INPUT_SIZE))
    throw std::length_error("message too large for LZ4");
  std::vector<char> frame(kHeader);
  std::memcpy(frame.data(), kMagic, 4);
  frame[4] = static_cast<char>((shuffle ? kShuffle : 0) | (prev ? kXor : 0));
  std::memcpy(frame.data() + 8, &n, 8);
  if (n == 0)
    return frame;
  std::vector<char> a(cur, cur + n), b;
  if (prev)
    for (uint64_t i = 0; i < n; ++i)
      a[i] = static_cast<char>(a[i] ^ prev[i]);
  const char* src = a.data();
  if (shuffle) {
    b.resize(n);
    Shuffle(a.data(), b.data(), n);
    src = b.data();
  }
  const int bound = LZ4_compressBound(static_cast<int>(n));
  frame.resize(kHeader + static_cast<uint64_t>(bound));
  const int z = LZ4_compress_default(src, frame.data() + kHeader,
                                     static_cast<int>(n), bound);
  if (z <= 0)
    throw std::runtime_error("LZ4 compression failed");
  frame.resize(kHeader + static_cast<uint64_t>(z));
  return frame;
}

Status DecodeDelta(const char* frame, uint64_t frame_size, const char* prev,
                   uint64_t prev_size, std::vector<char>& out) {
  out.clear();
  if (frame_size < kHeader)
    return Status::kTruncated;
  if (std::memcmp(frame, kMagic, 4) != 0)
    return Status::kBadHeader;
  const auto flags = static_cast<uint8_t>(frame[4]);
  if ((flags & ~(kShuffle | kXor)) != 0 || frame[5] != 0 || frame[6] != 0 ||
      frame[7] != 0)
    return Status::kBadHeader;
  uint64_t n;
  std::memcpy(&n, frame + 8, 8);
  const uint64_t payload = frame_size - kHeader;
  if (n > static_cast<uint64_t>(LZ4_MAX_INPUT_SIZE) ||
      payload > static_cast<uint64_t>(INT_MAX))
    return Status::kBadHeader;
  if ((flags & kXor) && (prev == nullptr || prev_size != n))
    return Status::kSizeMismatch;
  if (n == 0)
    return payload == 0 ? Status::kOk : Status::kCorrupt;
  if (n > payload * 256 + 64)
    return Status::kCorrupt;  // beyond LZ4's maximum ratio: forged size
  out.resize(n);
  std::vector<char> tmp((flags & kShuffle) ? n : 0);
  char* dst = (flags & kShuffle) ? tmp.data() : out.data();
  const int r = LZ4_decompress_safe(
      frame + kHeader, dst, static_cast<int>(payload), static_cast<int>(n));
  if (r < 0 || static_cast<uint64_t>(r) != n) {
    out.clear();
    return Status::kCorrupt;
  }
  if (flags & kShuffle)
    Unshuffle(tmp.data(), out.data(), n);
  if (flags & kXor)
    for (uint64_t i = 0; i < n; ++i)
      out[i] = static_cast<char>(out[i] ^ prev[i]);
  return Status::kOk;
}

}  // namespace abmio
