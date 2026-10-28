// SPDX-License-Identifier: Apache-2.0
// Statistical regression tests for the border-hoarding findings
// (docs/FINDINGS.md). Thresholds keep wide margins against seed noise; tighten
// them as the model grows.
#include <gtest/gtest.h>

#include "physics/wall_model.h"

using physics::Chemo;

using physics::Wall;

TEST(BorderHoarding, ClampingPilesAgentsAtWalls) {
  const auto m = physics::Run(Wall::kClamp, Chemo::kNone);
  EXPECT_GT(m.near_wall, 1.8 * m.uniform_near_wall);
  EXPECT_EQ(m.n_end, m.n_start);
}
TEST(BorderHoarding, ReflectionReducesTheExcess) {
  EXPECT_LT(physics::Run(Wall::kReflect, Chemo::kNone).near_wall,
            physics::Run(Wall::kClamp, Chemo::kNone).near_wall);
}
TEST(BorderHoarding, ContactForceRemovesTheExcess) {
  const auto m = physics::Run(Wall::kContact, Chemo::kNone);
  EXPECT_LT(m.near_wall, 0.2 * m.uniform_near_wall);
  EXPECT_EQ(m.n_end, m.n_start);
}
TEST(BorderHoarding, DeletingAtTheWallLosesAgents) {
  const auto m = physics::Run(Wall::kAbsorb, Chemo::kNone);
  EXPECT_LT(static_cast<double>(m.n_end), 0.8 * static_cast<double>(m.n_start));
}
TEST(BorderHoarding, NormalizedChemotaxisOnAFlatGradientHoards) {
  const auto m = physics::Run(Wall::kClamp, Chemo::kNormalized);
  EXPECT_GT(m.first_layer, 4 * m.uniform_first_layer);
}
TEST(BorderHoarding, ProportionalChemotaxisDoesNot) {
  const auto m = physics::Run(Wall::kClamp, Chemo::kProportional);
  EXPECT_LT(m.first_layer, 1.6 * m.uniform_first_layer);
}
TEST(BorderHoarding, WallsAloneDoNotFixNormalizedChemotaxis) {
  const auto m = physics::Run(Wall::kContact, Chemo::kNormalized);
  EXPECT_GT(m.first_layer, 3 * m.uniform_first_layer);
}
