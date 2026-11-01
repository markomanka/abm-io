// SPDX-License-Identifier: Apache-2.0
// A user's model on top of the miniature BioDynaMo, in a header of its own.
#pragma once
#include <vector>

#include "bdm_like.h"

namespace model {

class MyCell : public bdm::Cell {
  BDM_AGENT_HEADER(MyCell, Cell, 1);

 private:
  int cell_type_ = 0;
  std::vector<double> concentrations_;
  double cached_pressure_ = 0;  //!
};

}  // namespace model
