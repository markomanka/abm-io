// SPDX-License-Identifier: Apache-2.0
// The harness of docs/RATIONALE.md entry 12: writes the same checkpoint with
// every binding this build has, with and without the LZ4 filter, reads every
// file back with every binding, and reports wall time, throughput, peak memory
// and file size per phase. Each phase runs in a child process, so its peak
// resident size is its own and the chunk-bounded writer's memory is visible.
//
//   abmio_h5binding_compare [--agents N] [--chunk C] [--cache BYTES] [--dir D]
//
// Prints one line per phase, tab-separated, and exits non-zero if any phase
// failed or any agent read back differently from the one written. Timings
// from a shared machine are indicative only; the decision uses the dedicated
// benchmark machine (docs/QUALITY-GATES.md).
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "binding.h"

namespace {

using abmio::h5binding::Binding;
using abmio::h5binding::Layout;
using abmio::h5binding::ReadResult;
using abmio::h5binding::Records;

struct Phase {
  std::string what;
  double seconds = 0;
  long peak_kib = 0;
  int status = 0;
  uint64_t bytes = 0;
};

// Runs `f` in a child and reports its wall time, exit status and peak RSS.
template <class F>
Phase InChild(const std::string& what, F f) {
  Phase p;
  p.what = what;
  const auto start = std::chrono::steady_clock::now();
  const pid_t pid = fork();
  if (pid == 0) {
    _exit(f() ? 0 : 1);
  }
  int status = 0;
  struct rusage usage;
  wait4(pid, &status, 0, &usage);
  p.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  p.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#ifdef __APPLE__
  p.peak_kib = usage.ru_maxrss / 1024;  // bytes on macOS
#else
  p.peak_kib = usage.ru_maxrss;  // kibibytes on Linux
#endif
  return p;
}

uint64_t FileSize(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Layout layout;
  layout.agents = 1000000;
  layout.chunk = 16384;
  std::string dir = ".";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "--agents" && value) {
      layout.agents = std::strtoull(value, nullptr, 10);
      ++i;
    } else if (arg == "--chunk" && value) {
      layout.chunk = std::strtoull(value, nullptr, 10);
      ++i;
    } else if (arg == "--cache" && value) {
      layout.cache_bytes = std::strtoull(value, nullptr, 10);
      ++i;
    } else if (arg == "--dir" && value) {
      dir = value;
      ++i;
    } else {
      std::fprintf(stderr,
                   "usage: %s [--agents N] [--chunk C] [--cache BYTES] "
                   "[--dir D]\n",
                   argv[0]);
      return 2;
    }
  }
  const Records records = abmio::h5binding::SampleRecords();
  const std::vector<std::unique_ptr<Binding>> bindings =
      abmio::h5binding::Bindings();
  // About 44 bytes of agent state per agent: uid, position, diameter, type,
  // mother. The throughput column is this raw size over the phase's time.
  const double raw_mb = static_cast<double>(layout.agents) * 44 / 1e6;

  std::printf("agents\t%llu\tchunk\t%llu\tcache\t%zu\n",
              static_cast<unsigned long long>(layout.agents),
              static_cast<unsigned long long>(layout.chunk),
              layout.cache_bytes);
  std::printf("phase\tseconds\tMB/s\tpeak_MiB\tfile_MiB\tstatus\n");
  bool failed = false;
  std::vector<std::pair<std::string, std::string>> files;  // binding, path
  for (const bool lz4 : {false, true}) {
    Layout l = layout;
    l.lz4 = lz4;
    for (const auto& b : bindings) {
      const std::string path =
          dir + "/" + b->Name() + (lz4 ? "-lz4" : "-raw") + ".h5";
      Phase p = InChild(
          std::string("write\t") + b->Name() + (lz4 ? "\tlz4" : "\traw"), [&] {
            std::string why;
            const bool ok = b->Write(path, l, records, &why);
            if (!ok) {
              std::fprintf(stderr, "%s: %s\n", b->Name(), why.c_str());
            }
            return ok;
          });
      p.bytes = FileSize(path);
      std::printf("%s\t%.3f\t%.0f\t%.1f\t%.1f\t%d\n", p.what.c_str(), p.seconds,
                  raw_mb / p.seconds, p.peak_kib / 1024.0, p.bytes / 1048576.0,
                  p.status);
      failed = failed || p.status != 0;
      files.emplace_back(b->Name(), path);
    }
  }
  for (const auto& file : files) {
    const std::string& path = file.second;
    for (const auto& b : bindings) {
      Phase p = InChild(
          std::string("read\t") + b->Name() + "\t" +
              path.substr(dir.size() + 1),
          [&] {
            std::string why;
            ReadResult r;
            if (!b->Read(path, layout.chunk, &r, &why)) {
              std::fprintf(stderr, "%s: %s\n", b->Name(), why.c_str());
              return false;
            }
            if (r.agents != layout.agents || r.mismatches != 0 ||
                !(r.records == records)) {
              std::fprintf(stderr,
                           "%s reading %s: %llu agents, %llu "
                           "mismatches, records %s\n",
                           b->Name(), path.c_str(),
                           static_cast<unsigned long long>(r.agents),
                           static_cast<unsigned long long>(r.mismatches),
                           r.records == records ? "equal" : "differ");
              return false;
            }
            return true;
          });
      std::printf("%s\t%.3f\t%.0f\t%.1f\t%.1f\t%d\n", p.what.c_str(), p.seconds,
                  raw_mb / p.seconds, p.peak_kib / 1024.0,
                  FileSize(path) / 1048576.0, p.status);
      failed = failed || p.status != 0;
    }
  }
  return failed ? 1 : 0;
}
