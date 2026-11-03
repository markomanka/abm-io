// SPDX-License-Identifier: Apache-2.0
// The Rust decoder against the C++ one on the same inputs (milestone R).
// Both must give the same status and the same output, for frames the encoder
// produces and for random or corrupted bytes.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "abmio/codec.h"
#include "abmio/rust_codec.h"
#include "abmio/status.h"

namespace abmio {
namespace {

struct Decoded {
  int status;
  std::vector<char> out;
  bool operator==(const Decoded& other) const {
    return status == other.status && out == other.out;
  }
};

Decoded ByCpp(const std::vector<char>& frame,
              const std::vector<char>& previous) {
  std::vector<char> out;
  const Status s = DecodeDelta(frame.data(), frame.size(),
                               previous.empty() ? nullptr : previous.data(),
                               previous.size(), out);
  return {static_cast<int>(s), out};
}

Decoded ByRust(const std::vector<char>& frame,
               const std::vector<char>& previous) {
  std::vector<uint8_t> buffer(1 << 20);
  std::size_t size = 0;
  const uint8_t s = abmio_rust_decode_delta(
      reinterpret_cast<const uint8_t*>(frame.data()), frame.size(),
      previous.empty() ? nullptr
                       : reinterpret_cast<const uint8_t*>(previous.data()),
      previous.size(), buffer.data(), buffer.size(), &size);
  std::vector<char> out;
  if (s == static_cast<uint8_t>(Status::kOk)) {
    out.assign(buffer.begin(),
               buffer.begin() + static_cast<std::ptrdiff_t>(size));
  }
  return {s, out};
}

// Data of a given shape, so the encoder produces blocks of every kind.
std::vector<char> Shaped(int kind, std::size_t n, std::mt19937_64& rng) {
  std::vector<char> data(n);
  for (std::size_t i = 0; i < n; ++i) {
    data[i] = kind == 0   ? static_cast<char>(rng())        // incompressible
              : kind == 1 ? static_cast<char>('a' + i % 3)  // very compressible
              : kind == 2 ? static_cast<char>(i % 251)      // a ramp
                          : 0;                              // all zeros
  }
  return data;
}

TEST(RustCodec, FramesFromTheEncoderDecodeIdentically) {
  std::mt19937_64 rng(12345);
  int decoded = 0;
  for (int trial = 0; trial < 400; ++trial) {
    const std::size_t n = rng() % 3000;
    const std::vector<char> data = Shaped(trial % 4, n, rng);
    const std::vector<char> previous = Shaped(3 - trial % 4, n, rng);
    for (bool shuffle : {false, true}) {
      for (bool xored : {false, true}) {
        const std::vector<char> base = xored ? previous : std::vector<char>();
        const std::vector<char> frame = EncodeDelta(
            data.data(), n, xored ? previous.data() : nullptr, shuffle);
        const Decoded c = ByCpp(frame, base);
        const Decoded r = ByRust(frame, base);
        ASSERT_EQ(c.status, static_cast<int>(Status::kOk))
            << "the C++ decoder rejected a frame it had just encoded";
        ASSERT_EQ(c.out, data);
        ASSERT_TRUE(c == r)
            << "n=" << n << " shuffle=" << shuffle << " xor=" << xored
            << ": C++ " << c.status << "/" << c.out.size() << ", Rust "
            << r.status << "/" << r.out.size();
        ++decoded;
      }
    }
  }
  EXPECT_EQ(decoded, 1600);
}

TEST(RustCodec, RandomAndCorruptedBytesAreRejectedTheSameWay) {
  std::mt19937_64 rng(67890);
  int rejected = 0;
  for (int trial = 0; trial < 20000; ++trial) {
    std::vector<char> frame(16 + rng() % 200);
    if (trial % 3 == 0) {  // a plausible header over a random payload
      std::memcpy(frame.data(), "ABDZ", 4);
      frame[4] = static_cast<char>(rng() % 4);
      frame[5] = frame[6] = frame[7] = 0;
      const uint64_t n = rng() % 4000;
      std::memcpy(frame.data() + 8, &n, 8);
      for (std::size_t i = 16; i < frame.size(); ++i) {
        frame[i] = static_cast<char>(rng());
      }
    } else {
      for (char& b : frame) {
        b = static_cast<char>(rng());
      }
      if (trial % 3 == 1) {
        std::memcpy(frame.data(), "ABDZ", 4);
      }
    }
    uint64_t claimed = 0;
    std::memcpy(&claimed, frame.data() + 8, 8);
    const std::vector<char> previous(claimed <= 4000 ? claimed : 0, 0x5a);
    const Decoded c = ByCpp(frame, previous);
    const Decoded r = ByRust(frame, previous);
    ASSERT_TRUE(c == r) << "trial " << trial << ", size " << frame.size()
                        << ": C++ " << c.status << "/" << c.out.size()
                        << ", Rust " << r.status << "/" << r.out.size();
    if (c.status != static_cast<int>(Status::kOk)) {
      ++rejected;
    }
  }
  EXPECT_GT(rejected, 19000) << "almost every random input should be rejected";
}

TEST(RustCodec, TheCommittedFuzzingSeedsDecodeIdentically) {
  for (const std::string name : {"plain", "shuffled", "xor_shuffled"}) {
    const std::string path =
        std::string(ABMIO_CORPUS_DIR) + "/fuzz_delta/" + name;
    FILE* f = std::fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr) << "cannot read " << path;
    std::vector<char> frame;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof buffer, f)) > 0) {
      frame.insert(frame.end(), buffer, buffer + got);
    }
    std::fclose(f);
    ASSERT_GE(frame.size(), 16u);
    uint64_t claimed = 0;
    std::memcpy(&claimed, frame.data() + 8, 8);
    const std::vector<char> previous(claimed <= (1u << 20) ? claimed : 0, 0x5a);
    EXPECT_TRUE(ByCpp(frame, previous) == ByRust(frame, previous)) << name;
  }
}

}  // namespace
}  // namespace abmio
