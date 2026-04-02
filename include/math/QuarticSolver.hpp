#pragma once

#include <vector>

namespace torirender::math {

std::vector<double> solveQuarticReal(
    double a4, double a3, double a2, double a1, double a0, double tolerance = 1e-10);

}
