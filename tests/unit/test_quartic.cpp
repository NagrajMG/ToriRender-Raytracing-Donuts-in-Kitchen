#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "math/QuarticSolver.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("Quartic solver returns expected real roots", "[quartic]") {
  std::vector<double> roots = torirender::math::solveQuarticReal(1.0, 0.0, -5.0, 0.0, 4.0);
  std::sort(roots.begin(), roots.end());

  REQUIRE(roots.size() == 4);
  REQUIRE_THAT(roots[0], WithinAbs(-2.0, 1e-6));
  REQUIRE_THAT(roots[1], WithinAbs(-1.0, 1e-6));
  REQUIRE_THAT(roots[2], WithinAbs(1.0, 1e-6));
  REQUIRE_THAT(roots[3], WithinAbs(2.0, 1e-6));
}

TEST_CASE("Quartic solver excludes complex-only roots", "[quartic]") {
  const std::vector<double> roots = torirender::math::solveQuarticReal(1.0, 0.0, 0.0, 0.0, 1.0);
  REQUIRE(roots.empty());
}
