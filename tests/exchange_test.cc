// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <stdexcept>
#include <tuple>

#include "abmio/exchange.h"
#include "test_util.h"

using abmio::Arena;
using abmio::Layout;
using abmio::Status;

class RoundTrip
    : public ::testing::TestWithParam<std::tuple<Layout, size_t, uint32_t>> {};

TEST_P(RoundTrip, DecodesExactlyWhatWasPacked) {
  const auto [layout, n, block] = GetParam();
  auto pop = abmio::test::MakePopulation(n, 1234 + n);
  std::vector<char> buf;
  const uint64_t len = abmio::Pack(pop.ptrs, layout, buf, block);
  Arena arena;
  ASSERT_EQ(abmio::Unpack(buf.data(), len, arena), Status::kOk);
  std::string why;
  EXPECT_EQ(abmio::test::CountMismatches(pop.ptrs, arena.view, &why), 0u)
      << why;
}

INSTANTIATE_TEST_SUITE_P(
    AllLayouts, RoundTrip,
    ::testing::Combine(::testing::Values(Layout::kRows, Layout::kColumns),
                       ::testing::Values(size_t{0}, size_t{1}, size_t{1023},
                                         size_t{1024}, size_t{1025},
                                         size_t{5000}),
                       ::testing::Values(1u, 7u, 1024u, 65535u)));

TEST(Exchange, ReusesArenaAcrossMessagesOfDifferentShape) {
  Arena arena;
  std::vector<char> buf;
  for (int round = 0; round < 6; ++round) {
    const bool odd = round % 2 == 1;
    auto pop = abmio::test::MakePopulation(odd ? 300 : 1200, round,
                                           odd ? 0.9 : 0.1, round % 3 == 0);
    const uint64_t len =
        abmio::Pack(pop.ptrs, odd ? Layout::kRows : Layout::kColumns, buf, 256);
    ASSERT_EQ(abmio::Unpack(buf.data(), len, arena), Status::kOk);
    std::string why;
    EXPECT_EQ(abmio::test::CountMismatches(pop.ptrs, arena.view, &why), 0u)
        << "round " << round << ": " << why;
  }
}

TEST(Exchange, RejectsEveryTruncation) {
  auto pop = abmio::test::MakePopulation(40, 9);
  for (Layout layout : {Layout::kRows, Layout::kColumns}) {
    std::vector<char> buf;
    const uint64_t len = abmio::Pack(pop.ptrs, layout, buf, 16);
    Arena arena;
    for (uint64_t cut = 0; cut < len; ++cut) {
      std::vector<char> part(
          buf.begin(),
          buf.begin() +
              static_cast<long>(cut));  // exact size: ASan sees overreads
      EXPECT_NE(abmio::Unpack(part.data(), cut, arena), Status::kOk)
          << "cut at " << cut;
    }
  }
}

TEST(Exchange, RandomCorruptionIsRejectedOrDecodedSafely) {
  auto pop = abmio::test::MakePopulation(200, 5);
  std::mt19937_64 rng(77);
  for (Layout layout : {Layout::kRows, Layout::kColumns}) {
    std::vector<char> buf;
    const uint64_t len = abmio::Pack(pop.ptrs, layout, buf, 64);
    Arena arena;
    for (int trial = 0; trial < 3000; ++trial) {
      std::vector<char> m(buf.begin(), buf.begin() + static_cast<long>(len));
      const int flips = 1 + static_cast<int>(rng() % 4);
      for (int f = 0; f < flips; ++f)
        m[rng() % len] ^= static_cast<char>(1 + rng() % 255);
      if (abmio::Unpack(m.data(), len, arena) == Status::kOk) {
        EXPECT_EQ(arena.view.size(), pop.ptrs.size());
      }
    }
  }
}

TEST(Exchange, RejectsBadHeaders) {
  auto pop = abmio::test::MakePopulation(10, 3);
  std::vector<char> buf;
  const uint64_t len = abmio::Pack(pop.ptrs, Layout::kRows, buf);
  Arena arena;
  auto patched = [&](size_t at, const void* bytes, size_t n) {
    std::vector<char> m(buf.begin(), buf.begin() + static_cast<long>(len));
    std::memcpy(m.data() + at, bytes, n);
    return abmio::Unpack(m.data(), len, arena);
  };
  const char x = 'X', v9 = 9, l7 = 7;
  const uint64_t huge = ~uint64_t{0};
  EXPECT_EQ(patched(0, &x, 1), Status::kBadHeader);   // magic
  EXPECT_EQ(patched(4, &v9, 1), Status::kBadHeader);  // version
  EXPECT_EQ(patched(5, &l7, 1), Status::kBadHeader);  // layout
  EXPECT_EQ(patched(8, &huge, 8),
            Status::kBadHeader);  // agent count beyond the buffer
  std::vector<char> longer(buf.begin(), buf.begin() + static_cast<long>(len));
  longer.push_back(0);
  EXPECT_EQ(abmio::Unpack(longer.data(), longer.size(), arena),
            Status::kSizeMismatch);
}

TEST(Exchange, PackRejectsUnregisteredTypesAndOverfullAgents) {
  std::vector<char> buf;
  abmio::Agent plain;  // type id 0 is not registered
  EXPECT_THROW(abmio::Pack({&plain}, Layout::kRows, buf),
               std::invalid_argument);
  abmio::Cell crowded;
  for (int i = 0; i < 256; ++i)
    crowded.behaviors.push_back(new abmio::Growth);
  EXPECT_THROW(abmio::Pack({&crowded}, Layout::kRows, buf),
               std::invalid_argument);
  abmio::Cell ok;
  EXPECT_THROW(abmio::Pack({&ok}, Layout::kRows, buf, 0),
               std::invalid_argument);
}

TEST(Status, EveryCodeHasAReadableName) {
  EXPECT_STREQ(abmio::ToString(Status::kOk), "ok");
  for (int i = 0; i <= static_cast<int>(Status::kCorrupt); ++i) {
    EXPECT_STRNE(abmio::ToString(static_cast<Status>(i)), "unknown");
  }
}
