#pragma once

#include <array>
#include <vector>

#include "core/Accel.hpp"

namespace torirender::math {

struct QuarticRealRoots {
  std::array<double, 4> values{};
  int count = 0;
};

TORIRENDER_ACC_ROUTINE_SEQ
QuarticRealRoots solveQuarticRealFixed(
    double a4, double a3, double a2, double a1, double a0, double tolerance = 1e-10);

std::vector<double> solveQuarticReal(
    double a4, double a3, double a2, double a1, double a0, double tolerance = 1e-10);

}  // namespace torirender::math
