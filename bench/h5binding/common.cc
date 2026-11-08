// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include <string>
#include <vector>

#include "binding.h"

namespace abmio {
namespace h5binding {

Records SampleRecords() {
  Records r;
  r.simulation.step = 1200;
  r.simulation.time = 1200.0 * 0.01;
  r.simulation.time_unit = "h";
  ComponentRecord c;
  c.name = "biodynamo";
  c.version = "1.06";
  c.role = "simulator";
  c.digest = "";
  c.affects_results = true;
  c.reference = "https://github.com/markomanka/biodynamo";
  r.components.push_back(c);
  c.name = "libroadrunner";
  c.version = "2.7.0";
  c.role = "solver";
  c.digest =
      "sha256:0000000000000000000000000000000000000000000000000000000000000000";
  c.affects_results = true;
  c.reference = "";
  r.components.push_back(c);
  c.name = "paraview";
  c.version = "6.2.0";
  c.role = "library";
  c.digest = "";
  c.affects_results = false;
  c.reference = "";
  r.components.push_back(c);
  for (uint64_t t = 0; t < 8; ++t) {
    RandomStreamRecord s;
    s.owner = "thread " + std::to_string(t);
    s.key = 0x9E3779B97F4A7C15ULL ^ t;
    s.counter = 1000 * (t + 1);
    r.streams.push_back(s);
  }
  return r;
}

bool operator==(const Records& a, const Records& b) {
  if (a.simulation.step != b.simulation.step ||
      a.simulation.time != b.simulation.time ||
      a.simulation.time_unit != b.simulation.time_unit ||
      a.components.size() != b.components.size() ||
      a.streams.size() != b.streams.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.components.size(); ++i) {
    const ComponentRecord& x = a.components[i];
    const ComponentRecord& y = b.components[i];
    if (x.name != y.name || x.version != y.version || x.role != y.role ||
        x.digest != y.digest || x.affects_results != y.affects_results ||
        x.reference != y.reference) {
      return false;
    }
  }
  for (std::size_t i = 0; i < a.streams.size(); ++i) {
    const RandomStreamRecord& x = a.streams[i];
    const RandomStreamRecord& y = b.streams[i];
    if (x.owner != y.owner || x.key != y.key || x.counter != y.counter) {
      return false;
    }
  }
  return true;
}

std::vector<std::unique_ptr<Binding>> Bindings() {
  std::vector<std::unique_ptr<Binding>> all;
  all.push_back(CApiBinding());
#ifdef ABMIO_HAVE_HIGHFIVE
  all.push_back(HighFiveBinding());
#endif
  return all;
}

}  // namespace h5binding
}  // namespace abmio
