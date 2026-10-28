// SPDX-License-Identifier: Apache-2.0
// Driver for compilers without libFuzzer (e.g. GCC): runs every input file or
// directory given, then random mutations of each. Usage: fuzz_x corpus/
// [-runs=N]
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv) {
  int mutations = 2000;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("-runs=", 0) == 0) {
      mutations = std::stoi(a.substr(6));
    } else if (std::filesystem::is_directory(a)) {
      for (const auto& e : std::filesystem::directory_iterator(a))
        files.push_back(e.path().string());
    } else {
      files.push_back(a);
    }
  }
  std::mt19937_64 rng(1);
  size_t runs = 0;
  for (const auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    LLVMFuzzerTestOneInput(d.data(), d.size());
    ++runs;
    for (int m = 0; m < mutations && !d.empty(); ++m) {
      auto c = d;
      const int flips = 1 + static_cast<int>(rng() % 8);
      for (int k = 0; k < flips; ++k)
        c[rng() % c.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
      if (rng() % 4 == 0)
        c.resize(rng() % (c.size() + 1));
      LLVMFuzzerTestOneInput(c.data(), c.size());
      ++runs;
    }
  }
  std::printf("replayed %zu inputs\n", runs);
  return 0;
}
