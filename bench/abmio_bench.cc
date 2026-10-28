// SPDX-License-Identifier: Apache-2.0
// Benchmarks the default exchange path and prints JSON for
// scripts/compare_bench.py. Usage: abmio_bench [--quick]
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "abmio/codec.h"
#include "abmio/exchange.h"
#include "test_util.h"

namespace {
double Now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
template <class F>
double MedianSeconds(int reps, F&& f) {
  std::vector<double> t;
  for (int r = 0; r < reps; ++r) {
    const double a = Now();
    f();
    t.push_back(Now() - a);
  }
  std::sort(t.begin(), t.end());
  return t[t.size() / 2];
}
// Minimum work of any serializer that reads object state: copy every reachable
// object once.
uint64_t Gather(const std::vector<abmio::Agent*>& ag, std::vector<char>& out) {
  uint64_t pos = 0;
  for (const abmio::Agent* a : ag) {
    if (out.size() < pos + 512)
      out.resize(2 * (pos + 512));
    const size_t sa =
        a->TypeId() == 1 ? sizeof(abmio::Cell) : sizeof(abmio::MyCell);
    std::memcpy(out.data() + pos, static_cast<const void*>(a), sa);
    pos += sa;
    for (const abmio::Behavior* b : a->behaviors) {
      const size_t sb =
          b->TypeId() == 1 ? sizeof(abmio::Growth) : sizeof(abmio::Secretion);
      std::memcpy(out.data() + pos, static_cast<const void*>(b), sb);
      pos += sb;
    }
  }
  return pos;
}
}  // namespace

int main(int argc, char** argv) {
  const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
  const std::vector<size_t> sizes =
      quick ? std::vector<size_t>{20000}
            : std::vector<size_t>{20000, 200000, 1000000};
  const int reps = quick ? 3 : 7;
  std::map<std::string, double> r;
  for (size_t n : sizes) {
    const std::string k = "n" + std::to_string(n) + ".";
    auto pop = abmio::test::MakePopulation(n, 42);
    const double per = 1e9 / static_cast<double>(n);
    std::vector<char> scratch;
    r[k + "gather_ns"] =
        per * MedianSeconds(reps, [&] { Gather(pop.ptrs, scratch); });
    for (auto layout : {abmio::Layout::kRows, abmio::Layout::kColumns}) {
      const std::string l =
          k + (layout == abmio::Layout::kRows ? "rows." : "columns.");
      std::vector<char> buf;
      uint64_t len = abmio::Pack(pop.ptrs, layout, buf);
      r[l + "pack_ns"] = per * MedianSeconds(reps, [&] {
                           len = abmio::Pack(pop.ptrs, layout, buf);
                         });
      abmio::Arena arena;
      if (abmio::Unpack(buf.data(), len, arena) != abmio::Status::kOk)
        return 1;
      r[l + "unpack_ns"] = per * MedianSeconds(reps, [&] {
                             abmio::Unpack(buf.data(), len, arena);
                           });
      r[l + "bytes_per_agent"] =
          static_cast<double>(len) / static_cast<double>(n);
    }
    std::vector<char> a, b;
    const uint64_t na = abmio::Pack(pop.ptrs, abmio::Layout::kColumns, a);
    abmio::test::Drift(pop, 43);
    const uint64_t nb = abmio::Pack(pop.ptrs, abmio::Layout::kColumns, b);
    if (na != nb)
      return 1;
    std::vector<char> frame, out;
    r[k + "delta.encode_ns"] =
        per * MedianSeconds(reps, [&] {
          frame = abmio::EncodeDelta(b.data(), nb, a.data(), true);
        });
    r[k + "delta.decode_ns"] =
        per * MedianSeconds(reps, [&] {
          abmio::DecodeDelta(frame.data(), frame.size(), a.data(), na, out);
        });
    r[k + "delta.bytes_per_agent"] =
        static_cast<double>(frame.size()) / static_cast<double>(n);
    r[k + "lz4.bytes_per_agent"] =
        static_cast<double>(
            abmio::EncodeDelta(b.data(), nb, nullptr, false).size()) /
        static_cast<double>(n);
  }
  char host[256] = "unknown";
  gethostname(host, sizeof(host) - 1);
  std::printf("{\n  \"schema\": 1,\n  \"host\": \"%s\",\n  \"results\": {\n",
              host);
  size_t i = 0;
  for (const auto& [key, value] : r)
    std::printf("    \"%s\": %.3f%s\n", key.c_str(), value,
                ++i < r.size() ? "," : "");
  std::printf("  }\n}\n");
  return 0;
}
