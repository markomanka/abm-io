// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>

#include "abmio/codec.h"
#include "abmio/exchange.h"
#include "test_util.h"

using abmio::Layout;
using abmio::Status;

namespace {
struct TwoIterations {
  std::vector<char> a, b;
  uint64_t na = 0, nb = 0;
};
TwoIterations Make() {
  TwoIterations t;
  auto pop = abmio::test::MakePopulation(3000, 11);
  t.na = abmio::Pack(pop.ptrs, Layout::kColumns, t.a);
  abmio::test::Drift(pop, 12);
  t.nb = abmio::Pack(pop.ptrs, Layout::kColumns, t.b);
  return t;
}
}  // namespace

TEST(Codec, RoundTripsInAllModes) {
  const auto t = Make();
  ASSERT_EQ(t.na, t.nb);
  for (bool shuffle : {false, true}) {
    for (bool with_prev : {false, true}) {
      const char* prev = with_prev ? t.a.data() : nullptr;
      const auto frame = abmio::EncodeDelta(t.b.data(), t.nb, prev, shuffle);
      std::vector<char> out;
      ASSERT_EQ(abmio::DecodeDelta(frame.data(), frame.size(), prev,
                                   with_prev ? t.na : 0, out),
                Status::kOk);
      ASSERT_EQ(out.size(), t.nb);
      EXPECT_TRUE(std::equal(out.begin(), out.end(), t.b.begin()));
    }
  }
}

TEST(Codec, DeltaAgainstPreviousShrinksDriftedMessages) {
  const auto t = Make();
  const size_t plain =
      abmio::EncodeDelta(t.b.data(), t.nb, nullptr, false).size();
  const size_t delta =
      abmio::EncodeDelta(t.b.data(), t.nb, t.a.data(), true).size();
  EXPECT_LT(2 * delta, plain)
      << "plain LZ4 " << plain << " bytes, delta " << delta << " bytes";
}

TEST(Codec, HandlesEmptyMessages) {
  const auto frame = abmio::EncodeDelta(nullptr, 0, nullptr, true);
  std::vector<char> out{'x'};
  EXPECT_EQ(abmio::DecodeDelta(frame.data(), frame.size(), nullptr, 0, out),
            Status::kOk);
  EXPECT_TRUE(out.empty());
}

TEST(Codec, RejectsTruncatedFramesAndSurvivesCorruption) {
  const auto t = Make();
  const auto frame = abmio::EncodeDelta(t.b.data(), t.nb, t.a.data(), true);
  std::vector<char> out;
  for (size_t cut = 0; cut < frame.size(); cut += 1 + cut / 64) {
    std::vector<char> part(frame.begin(),
                           frame.begin() + static_cast<long>(cut));
    EXPECT_NE(abmio::DecodeDelta(part.data(), cut, t.a.data(), t.na, out),
              Status::kOk)
        << "cut at " << cut;
  }
  std::mt19937_64 rng(3);
  for (int trial = 0; trial < 2000; ++trial) {
    auto m = frame;
    m[rng() % m.size()] ^= static_cast<char>(1 + rng() % 255);
    abmio::DecodeDelta(m.data(), m.size(), t.a.data(), t.na,
                       out);  // any status; must not crash
  }
}

TEST(Codec, RejectsWrongReferencesAndForgedHeaders) {
  const auto t = Make();
  auto frame = abmio::EncodeDelta(t.b.data(), t.nb, t.a.data(), false);
  std::vector<char> out;
  EXPECT_EQ(
      abmio::DecodeDelta(frame.data(), frame.size(), t.a.data(), t.na - 1, out),
      Status::kSizeMismatch);
  EXPECT_EQ(abmio::DecodeDelta(frame.data(), frame.size(), nullptr, 0, out),
            Status::kSizeMismatch);
  auto forged = frame;
  const uint64_t huge = uint64_t{1} << 30;
  std::memcpy(forged.data() + 8, &huge, 8);
  EXPECT_NE(abmio::DecodeDelta(forged.data(), forged.size(), nullptr, 0, out),
            Status::kOk);
  auto bad_magic = frame;
  bad_magic[0] = 'Q';
  EXPECT_EQ(abmio::DecodeDelta(bad_magic.data(), bad_magic.size(), t.a.data(),
                               t.na, out),
            Status::kBadHeader);
  auto bad_flags = frame;
  bad_flags[4] = static_cast<char>(0x80);
  EXPECT_EQ(abmio::DecodeDelta(bad_flags.data(), bad_flags.size(), t.a.data(),
                               t.na, out),
            Status::kBadHeader);
}
