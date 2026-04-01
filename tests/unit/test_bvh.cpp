#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "geometry/BVH.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("AABB bounds encapsulate Y-axis torus extents", "[aabb][torus]") {
  const torirender::Torus torus(
      2.0, 0.5, torirender::Vec3(1.0, 2.0, 3.0), torirender::TorusAxis::Y);
  const torirender::AABB bounds = torus.bounds();

  REQUIRE_THAT(bounds.min().x, WithinAbs(-1.5, 1e-12));
  REQUIRE_THAT(bounds.min().y, WithinAbs(1.5, 1e-12));
  REQUIRE_THAT(bounds.min().z, WithinAbs(0.5, 1e-12));

  REQUIRE_THAT(bounds.max().x, WithinAbs(3.5, 1e-12));
  REQUIRE_THAT(bounds.max().y, WithinAbs(2.5, 1e-12));
  REQUIRE_THAT(bounds.max().z, WithinAbs(5.5, 1e-12));
}

TEST_CASE("BVH node contains exactly two tori and their combined bounds", "[bvh]") {
  const torirender::Torus first(
      2.0, 0.5, torirender::Vec3(-3.0, 0.0, 0.0), torirender::TorusAxis::Y);
  const torirender::Torus second(
      1.5, 0.25, torirender::Vec3(4.0, 1.0, 0.0), torirender::TorusAxis::Y);

  const torirender::BVHNode node(first, second);

  REQUIRE(node.primitives().size() == 2);
  REQUIRE(node.bounds().contains(first.bounds()));
  REQUIRE(node.bounds().contains(second.bounds()));
}
