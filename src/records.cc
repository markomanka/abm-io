// SPDX-License-Identifier: Apache-2.0
#include "abmio/records.h"

#include <algorithm>
#include <map>

namespace abmio {
namespace {

std::string Describe(const ComponentRecord& c) {
  return c.role.empty() ? c.name : c.name + " (" + c.role + ")";
}

// One difference, with the name of the component it concerns kept apart from
// the message, so that acceptance compares names and never parses text.
struct Difference {
  std::string component;
  std::string message;
};

// Every difference that affects results. A component affects results if it
// is declared to on either side: one that was harmless when the checkpoint
// was written and is declared to affect results now still has to match, and
// one whose declaration was dropped in the same breath as a new version is
// not let through by the change of declaration alone.
std::vector<Difference> Differences(const std::vector<ComponentRecord>& before,
                                    const std::vector<ComponentRecord>& now) {
  std::map<std::string, const ComponentRecord*> current;
  for (const ComponentRecord& c : now) {
    current[c.name] = &c;
  }

  std::vector<Difference> out;
  for (const ComponentRecord& was : before) {
    const auto it = current.find(was.name);
    const ComponentRecord* is = it == current.end() ? nullptr : it->second;
    if (!was.affects_results && (is == nullptr || !is->affects_results)) {
      continue;  // a change here cannot change what is computed
    }
    if (is == nullptr) {
      out.push_back({was.name, Describe(was) + " " + was.version +
                                   " is no longer present"});
    } else if (is->version != was.version) {
      out.push_back({was.name, Describe(was) + " was " + was.version +
                                   ", is now " + is->version});
    } else if (!was.digest.empty() && !is->digest.empty() &&
               is->digest != was.digest) {
      out.push_back(
          {was.name, Describe(was) + " " + was.version +
                         " has the same version but different content"});
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
      out.push_back(
          {is.name, Describe(is) + " " + is.version +
                        " was not present when the checkpoint was written"});
    }
  }
  return out;
}

}  // namespace

bool ComponentsDiffer(const std::vector<ComponentRecord>& before,
                      const std::vector<ComponentRecord>& now,
                      std::vector<std::string>* differences) {
  const std::vector<Difference> all = Differences(before, now);
  if (differences != nullptr) {
    for (const Difference& d : all) {
      differences->push_back(d.message);
    }
  }
  return !all.empty();
}

bool BlockingDifferences(const std::vector<ComponentRecord>& before,
                         const std::vector<ComponentRecord>& now,
                         const std::vector<std::string>& accepted,
                         std::vector<std::string>* blocking) {
  // A difference is accepted only if its component was named exactly: a
  // prefix, a substring or a different case accepts nothing.
  bool blocked = false;
  for (const Difference& d : Differences(before, now)) {
    const bool named = std::find(accepted.begin(), accepted.end(),
                                 d.component) != accepted.end();
    if (!named) {
      blocked = true;
      if (blocking != nullptr) {
        blocking->push_back(d.message);
      }
    }
  }
  return blocked;
}

}  // namespace abmio
