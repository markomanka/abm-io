// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "abmio/model.h"

namespace abmio::test {

struct Population {
  std::vector<std::unique_ptr<Agent>> owned;
  std::vector<Agent*> ptrs;
};

// 70% MyCell, 30% Cell; every agent has a Growth behavior, `secretion_share` of
// them also a Secretion (placed first when `secretion_first`).
inline Population MakePopulation(size_t n, uint64_t seed,
                                 double secretion_share = 0.3,
                                 bool secretion_first = false) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> u(0, 1);
  Population p;
  p.owned.reserve(n);
  p.ptrs.reserve(n);
  for (size_t k = 0; k < n; ++k) {
    std::unique_ptr<Cell> c;
    if (u(rng) < 0.7) {
      auto m = std::make_unique<MyCell>();
      m->cell_type = static_cast<int>(k % 3);
      m->age = 100 * u(rng);
      c = std::move(m);
    } else {
      c = std::make_unique<Cell>();
    }
    c->uid = k;
    c->mother_uid = k / 2;
    for (double& x : c->pos)
      x = 1000 * u(rng);
    c->tractor[0] = u(rng) - 0.5;
    c->diameter = 8 + 4 * u(rng);
    c->volume = 3.14159265358979 / 6 * c->diameter * c->diameter * c->diameter;
    c->box_idx = static_cast<uint32_t>(c->pos[0] / 20) +
                 50 * static_cast<uint32_t>(c->pos[1] / 20) +
                 2500 * static_cast<uint32_t>(c->pos[2] / 20);
    auto* g = new Growth;
    g->threshold = 30 + 20 * u(rng);
    c->behaviors.push_back(g);
    if (u(rng) < secretion_share) {
      auto* s = new Secretion;
      s->substance = static_cast<int>(k % 4);
      s->quantity = u(rng);
      c->behaviors.insert(
          secretion_first ? c->behaviors.begin() : c->behaviors.end(), s);
    }
    p.ptrs.push_back(c.get());
    p.owned.push_back(std::move(c));
  }
  return p;
}

// One iteration of drift: positions random-walk, 10% of cells grow, ages
// advance.
inline void Drift(Population& p, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> step(0, 0.1);
  std::uniform_real_distribution<double> u(0, 1);
  for (Agent* a : p.ptrs) {
    auto* c = static_cast<Cell*>(a);
    for (double& x : c->pos)
      x += step(rng);
    if (u(rng) < 0.1)
      c->diameter += 0.01;
    if (a->TypeId() == 2)
      static_cast<MyCell*>(a)->age += 1;
  }
}

// Empty when equal, otherwise the first difference found.
inline std::string Compare(const Agent& a, const Agent& b) {
  if (a.TypeId() != b.TypeId())
    return "agent type";
  if (a.uid != b.uid || a.box_idx != b.box_idx)
    return "agent fields";
  const auto& x = static_cast<const Cell&>(a);
  const auto& y = static_cast<const Cell&>(b);
  for (int i = 0; i < 3; ++i)
    if (x.pos[i] != y.pos[i] || x.tractor[i] != y.tractor[i])
      return "cell vectors";
  if (x.diameter != y.diameter || x.volume != y.volume ||
      x.adherence != y.adherence || x.density != y.density ||
      x.mother_uid != y.mother_uid)
    return "cell fields";
  if (a.TypeId() == 2) {
    const auto& m = static_cast<const MyCell&>(a);
    const auto& n = static_cast<const MyCell&>(b);
    if (m.cell_type != n.cell_type || m.age != n.age)
      return "subclass fields";
  }
  if (a.behaviors.size() != b.behaviors.size())
    return "behavior count";
  for (size_t s = 0; s < a.behaviors.size(); ++s) {
    const Behavior* p = a.behaviors[s];
    const Behavior* q = b.behaviors[s];
    if (p == nullptr || q == nullptr || p->TypeId() != q->TypeId())
      return "behavior type";
    if (p->TypeId() == 1) {
      const auto& g = static_cast<const Growth&>(*p);
      const auto& h = static_cast<const Growth&>(*q);
      if (g.threshold != h.threshold || g.rate != h.rate)
        return "growth fields";
    } else {
      const auto& g = static_cast<const Secretion&>(*p);
      const auto& h = static_cast<const Secretion&>(*q);
      if (g.substance != h.substance || g.quantity != h.quantity)
        return "secretion fields";
    }
  }
  return {};
}

// Matches decoded agents to the source by uid; returns the number of
// mismatches.
inline size_t CountMismatches(const std::vector<Agent*>& src,
                              const std::vector<Agent*>& got,
                              std::string* first = nullptr) {
  if (src.size() != got.size()) {
    if (first)
      *first = "agent count";
    return src.size() + 1;
  }
  std::unordered_map<uint64_t, const Agent*> by_uid;
  for (const Agent* a : got)
    by_uid[a->uid] = a;
  size_t bad = 0;
  for (const Agent* a : src) {
    auto it = by_uid.find(a->uid);
    const std::string d =
        it == by_uid.end() ? "missing agent" : Compare(*a, *it->second);
    if (!d.empty()) {
      if (bad == 0 && first)
        *first = d;
      ++bad;
    }
  }
  return bad;
}

}  // namespace abmio::test
