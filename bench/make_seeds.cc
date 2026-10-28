// SPDX-License-Identifier: Apache-2.0
// Writes small valid messages and frames as fuzzing seeds: make_seeds
// <fuzz/corpus dir>
#include <fstream>
#include <string>
#include <vector>

#include "abmio/codec.h"
#include "abmio/exchange.h"
#include "test_util.h"

static void Write(const std::string& path, const char* d, size_t n) {
  std::ofstream(path, std::ios::binary)
      .write(d, static_cast<std::streamsize>(n));
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "fuzz/corpus";
  auto pop = abmio::test::MakePopulation(12, 1, 0.5, true);
  std::vector<char> rows, cols;
  const uint64_t nr = abmio::Pack(pop.ptrs, abmio::Layout::kRows, rows, 5);
  const uint64_t nc = abmio::Pack(pop.ptrs, abmio::Layout::kColumns, cols, 5);
  Write(dir + "/fuzz_unpack/rows_12_agents", rows.data(), nr);
  Write(dir + "/fuzz_unpack/columns_12_agents", cols.data(), nc);
  const auto plain = abmio::EncodeDelta(cols.data(), nc, nullptr, false);
  const auto shuffled = abmio::EncodeDelta(cols.data(), nc, nullptr, true);
  const auto xored = abmio::EncodeDelta(cols.data(), nc, rows.data(),
                                        true);  // reference of the same size
  Write(dir + "/fuzz_delta/plain", plain.data(), plain.size());
  Write(dir + "/fuzz_delta/shuffled", shuffled.data(), shuffled.size());
  if (nr == nc)
    Write(dir + "/fuzz_delta/xor_shuffled", xored.data(), xored.size());
  return 0;
}
