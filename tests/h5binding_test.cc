// SPDX-License-Identifier: Apache-2.0
// Every binding of bench/h5binding writes a checkpoint and reads it back,
// with and without the LZ4 filter, and each reads the others' files. The
// capabilities of docs/RATIONALE.md entry 12 are checked here, not timed.
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "abmio/h5lz4.h"
#include "h5binding/binding.h"

namespace abmio {
namespace h5binding {
namespace {

std::string PathFor(const Binding& b, bool lz4) {
  return std::string("abmio_h5binding_") + b.Name() + (lz4 ? "_lz4" : "_raw") +
         ".h5";
}

TEST(H5Binding, EachBindingRoundTripsWithAndWithoutTheFilter) {
  const Records records = SampleRecords();
  for (const auto& b : Bindings()) {
    for (const bool lz4 : {false, true}) {
      Layout layout;
      layout.agents = 10001;  // not a multiple of the chunk, on purpose
      layout.chunk = 1024;
      layout.lz4 = lz4;
      layout.cache_bytes = 4u << 20;
      const std::string path = PathFor(*b, lz4);
      std::string why;
      ASSERT_TRUE(b->Write(path, layout, records, &why)) << b->Name() << why;
      ReadResult r;
      ASSERT_TRUE(b->Read(path, 777, &r, &why)) << b->Name() << why;
      EXPECT_EQ(r.agents, layout.agents) << b->Name();
      EXPECT_EQ(r.mismatches, 0u) << b->Name();
      EXPECT_TRUE(r.records == records) << b->Name();
      if (lz4) {
        ASSERT_EQ(r.filters.size(), 1u) << b->Name();
        EXPECT_EQ(r.filters[0], kLz4FilterId) << b->Name();
      } else {
        EXPECT_TRUE(r.filters.empty()) << b->Name();
      }
    }
  }
}

TEST(H5Binding, EachBindingReadsTheOthersFiles) {
  const Records records = SampleRecords();
  const std::vector<std::unique_ptr<Binding>> bindings = Bindings();
  for (const bool lz4 : {false, true}) {
    for (const auto& writer : bindings) {
      Layout layout;
      layout.agents = 3000;
      layout.chunk = 512;
      layout.lz4 = lz4;
      const std::string path = PathFor(*writer, lz4);
      std::string why;
      ASSERT_TRUE(writer->Write(path, layout, records, &why)) << why;
      for (const auto& reader : bindings) {
        ReadResult r;
        ASSERT_TRUE(reader->Read(path, 512, &r, &why))
            << reader->Name() << " reading " << path << ": " << why;
        EXPECT_EQ(r.agents, layout.agents);
        EXPECT_EQ(r.mismatches, 0u) << reader->Name() << " reading " << path;
        EXPECT_TRUE(r.records == records)
            << reader->Name() << " reading " << path;
      }
    }
  }
  for (const auto& b : bindings) {
    std::remove(PathFor(*b, false).c_str());
    std::remove(PathFor(*b, true).c_str());
  }
}

}  // namespace
}  // namespace h5binding
}  // namespace abmio
