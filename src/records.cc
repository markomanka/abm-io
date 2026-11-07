// SPDX-License-Identifier: Apache-2.0
#include "abmio/records.h"

#include <algorithm>
#include <map>

namespace abmio {
namespace {

std::string Describe(const ComponentRecord& c) {
  return c.role.empty() ? c.name : c.name + " (" + c.role + ")";
}

}  // namespace

bool ComponentsDiffer(const std::vector<ComponentRecord>& before,
                      const std::vector<ComponentRecord>& now,
                      std::vector<std::string>* differences) {
  std::map<std::string, const ComponentRecord*> current;
  for (const ComponentRecord& c : now) {
    current[c.name] = &c;
  }

  bool differs = false;
  for (const ComponentRecord& was : before) {
    if (!was.affects_results) {
      continue;  // a change here cannot change what is computed
    }
    const auto it = current.find(was.name);
    if (it == current.end()) {
      differs = true;
      if (differences != nullptr) {
        differences->push_back(Describe(was) + " " + was.version +
                               " is no longer present");
      }
      continue;
    }
    const ComponentRecord& is = *it->second;
    if (is.version != was.version) {
      differs = true;
      if (differences != nullptr) {
        differences->push_back(Describe(was) + " was " + was.version +
                               ", is now " + is.version);
      }
    } else if (!was.digest.empty() && !is.digest.empty() &&
               is.digest != was.digest) {
      differs = true;
      if (differences != nullptr) {
        differences->push_back(Describe(was) + " " + was.version +
                               " has the same version but different content");
      }
    }
  }

  // Something new that affects results is also a difference: the checkpoint
  // was not written with it.
  for (const ComponentRecord& is : now) {
    if (!is.affects_results) {
      continue;
    }
    const bool known = std::any_of(
        before.begin(), before.end(),
        [&is](const ComponentRecord& was) { return was.name == is.name; });
    if (!known) {
      differs = true;
      if (differences != nullptr) {
        differences->push_back(
            Describe(is) + " " + is.version +
            " was not present when the checkpoint was written");
      }
    }
  }
  return differs;
}

}  // namespace abmio
