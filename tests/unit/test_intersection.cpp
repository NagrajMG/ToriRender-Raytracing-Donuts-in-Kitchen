#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "geometry/RayMarcher.hpp"
#include "geometry/RayTorus.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("Ray-torus quartic intersection finds closest positive hit", "[intersection][quartic]") {
  const torirender::Torus torus(2.0, 0.5);
  const torirender::Ray ray(torirender::Vec3(0.0, 0.0, -5.0), torirender::Vec3(0.0, 0.0, 1.0));

  torirender::HitRecord hit{};
  const bool didHit = torirender::intersectRayTorus(ray, torus, hit, 1e-4, 100.0, false);

  REQUIRE(didHit);
  REQUIRE_THAT(hit.t, WithinAbs(2.5, 1e-4));
  REQUIRE_THAT(hit.point.x, WithinAbs(0.0, 1e-6));
  REQUIRE_THAT(hit.point.y, WithinAbs(0.0, 1e-6));
  REQUIRE_THAT(hit.point.z, WithinAbs(-2.5, 1e-4));
  REQUIRE_THAT(hit.normal.z, WithinAbs(-1.0, 1e-3));
}

TEST_CASE("Ray marcher agrees with quartic hit distance on simple ray", "[intersection][marcher]") {
  const torirender::Torus torus(2.0, 0.5);
  const torirender::Ray ray(torirender::Vec3(0.0, 0.0, -5.0), torirender::Vec3(0.0, 0.0, 1.0));

  torirender::HitRecord quarticHit{};
  torirender::HitRecord marchedHit{};

  REQUIRE(torirender::intersectRayTorus(ray, torus, quarticHit, 1e-4, 100.0, false));
  REQUIRE(torirender::marchRayToTorus(ray, torus, marchedHit, 1e-4, 100.0, 256, 1e-5));

  REQUIRE_THAT(marchedHit.t, WithinAbs(quarticHit.t, 3e-3));
}
