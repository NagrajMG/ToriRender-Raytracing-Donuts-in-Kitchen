#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "math/Vec3.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("Vec3 addition", "[vec3]") {
  const torirender::Vec3 a(1.0, 2.0, 3.0);
  const torirender::Vec3 b(4.0, 5.0, 6.0);

  const torirender::Vec3 c = a + b;

  REQUIRE_THAT(c.x, WithinAbs(5.0, 1e-12));
  REQUIRE_THAT(c.y, WithinAbs(7.0, 1e-12));
  REQUIRE_THAT(c.z, WithinAbs(9.0, 1e-12));
}

TEST_CASE("Vec3 dot product", "[vec3]") {
  const torirender::Vec3 a(1.0, 2.0, 3.0);
  const torirender::Vec3 b(4.0, -5.0, 6.0);

  const double result = torirender::dot(a, b);

  REQUIRE_THAT(result, WithinAbs(12.0, 1e-12));
}

TEST_CASE("Vec3 cross product", "[vec3]") {
  const torirender::Vec3 a(1.0, 0.0, 0.0);
  const torirender::Vec3 b(0.0, 1.0, 0.0);

  const torirender::Vec3 c = torirender::cross(a, b);

  REQUIRE_THAT(c.x, WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(c.y, WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(c.z, WithinAbs(1.0, 1e-12));
}
