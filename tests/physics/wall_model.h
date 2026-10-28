// SPDX-License-Identifier: Apache-2.0
// Reduced border-hoarding model: soft spheres (10 um) with overlap repulsion,
// Gaussian motility and optional chemotaxis drift toward +x in a closed cube.
// See docs/FINDINGS.md.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

namespace physics {

enum class Wall {
  kClamp,
  kReflect,
  kContact,
  kAbsorb
};  // kClamp = BioDynaMo `closed`
enum class Chemo {
  kNone,
  kNormalized,
  kProportional
};  // kNormalized = BioDynaMo Chemotaxis

struct Metrics {
  size_t n_start = 0, n_end = 0;
  double near_wall = 0;  // share of centres within 1 um of any wall
  double first_layer =
      0;  // share of centres in the first cell layer at the +x wall
  double uniform_near_wall = 0, uniform_first_layer = 0;
};

inline Metrics Run(Wall wall, Chemo chemo, size_t n0 = 1000, double L = 120,
                   int steps = 300, unsigned seed = 7) {
  constexpr double kD = 10, kR = 5, kSigma = 1.0, kMaxDisp = 3.0, kSpeed = 0.5;
  constexpr int kMeasure = 100;
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> uni(0, L);
  std::normal_distribution<double> noise(0, kSigma);
  std::vector<double> x(3 * n0);
  for (double& v : x)
    v = uni(rng);
  const int nb = static_cast<int>(std::ceil(L / kD));
  auto box = [&](double v) {
    return std::min(std::max(static_cast<int>(v / kD), 0), nb - 1);
  };
  std::vector<int> head(static_cast<size_t>(nb) * nb * nb), next;
  std::vector<double> disp, kept;
  double near = 0, first = 0, samples = 0;
  for (int step = 0; step < steps; ++step) {
    const size_t n = x.size() / 3;
    std::fill(head.begin(), head.end(), -1);
    next.assign(n, -1);
    for (size_t i = 0; i < n; ++i) {
      const int b =
          box(x[3 * i]) + nb * (box(x[3 * i + 1]) + nb * box(x[3 * i + 2]));
      next[i] = head[static_cast<size_t>(b)];
      head[static_cast<size_t>(b)] = static_cast<int>(i);
    }
    disp.assign(3 * n, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const double* p = &x[3 * i];
      const int bx = box(p[0]), by = box(p[1]), bz = box(p[2]);
      for (int z = std::max(bz - 1, 0); z <= std::min(bz + 1, nb - 1); ++z)
        for (int y = std::max(by - 1, 0); y <= std::min(by + 1, nb - 1); ++y)
          for (int xx = std::max(bx - 1, 0); xx <= std::min(bx + 1, nb - 1);
               ++xx)
            for (int j = head[static_cast<size_t>(xx + nb * (y + nb * z))];
                 j != -1; j = next[static_cast<size_t>(j)]) {
              if (static_cast<size_t>(j) == i)
                continue;
              const double* q = &x[3 * static_cast<size_t>(j)];
              const double d0 = p[0] - q[0], d1 = p[1] - q[1], d2 = p[2] - q[2];
              const double r2 = d0 * d0 + d1 * d1 + d2 * d2;
              if (r2 >= kD * kD || r2 == 0)
                continue;
              const double r = std::sqrt(r2), f = 0.5 * (kD - r) / r;
              disp[3 * i] += f * d0;
              disp[3 * i + 1] += f * d1;
              disp[3 * i + 2] += f * d2;
            }
      if (wall == Wall::kContact)
        for (int k = 0; k < 3; ++k) {
          if (p[k] < kR)
            disp[3 * i + k] += kR - p[k];
          if (L - p[k] < kR)
            disp[3 * i + k] -= kR - (L - p[k]);
        }
    }
    kept.clear();
    for (size_t i = 0; i < n; ++i) {
      double* d = &disp[3 * i];
      const double m = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (m > kMaxDisp)
        for (int k = 0; k < 3; ++k)
          d[k] *= kMaxDisp / m;
      double p[3];
      for (int k = 0; k < 3; ++k)
        p[k] = x[3 * i + k] + d[k] + noise(rng);
      if (chemo == Chemo::kNormalized)
        p[0] += kSpeed;  // full speed for any gradient
      if (chemo == Chemo::kProportional)
        p[0] += kSpeed * (1e-9 / 1e-3);  // same, nearly flat gradient
      bool out = false;
      for (double& v : p) {
        switch (wall) {
          case Wall::kClamp:
            v = std::min(std::max(v, 0.0), L - 1e-10);
            break;
          case Wall::kAbsorb:
            out = out || v < 0 || v >= L;
            break;
          default:
            if (v < 0)
              v = -v;
            if (v >= L)
              v = 2 * L - v;
            v = std::min(std::max(v, 0.0), L - 1e-10);
        }
      }
      if (!out)
        kept.insert(kept.end(), p, p + 3);
    }
    x.swap(kept);
    if (step >= steps - kMeasure) {
      for (size_t i = 0; i < x.size() / 3; ++i) {
        const double* p = &x[3 * i];
        double h = L;
        for (int k = 0; k < 3; ++k)
          h = std::min({h, p[k], L - p[k]});
        near += h < 1 ? 1 : 0;
        first += p[0] > L - kD ? 1 : 0;
        samples += 1;
      }
    }
  }
  Metrics m;
  m.n_start = n0;
  m.n_end = x.size() / 3;
  m.near_wall = samples > 0 ? near / samples : 0;
  m.first_layer = samples > 0 ? first / samples : 0;
  m.uniform_near_wall = 1 - std::pow((L - 2) / L, 3);
  m.uniform_first_layer = kD / L;
  return m;
}

}  // namespace physics
