#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "geometry/Torus.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("Torus point classification via implicit function sign", "[torus]") {
  const torirender::Torus torus(2.0, 0.5);

  const torirender::Vec3 insidePoint(2.0, 0.0, 0.0);
  const torirender::Vec3 onSurfacePoint(2.5, 0.0, 0.0);
  const torirender::Vec3 outsidePoint(0.0, 0.0, 0.0);

  REQUIRE(torus.evaluate(insidePoint) < 0.0);
  REQUIRE(torus.evaluate(outsidePoint) > 0.0);
  REQUIRE_THAT(torus.evaluate(onSurfacePoint), WithinAbs(0.0, 1e-9));

  REQUIRE(torus.classify(insidePoint) == torirender::PointClassification::Inside);
  REQUIRE(torus.classify(onSurfacePoint) == torirender::PointClassification::On);
  REQUIRE(torus.classify(outsidePoint) == torirender::PointClassification::Outside);
}

TEST_CASE("Torus normal uses gradient and is unit length", "[torus]") {
  const torirender::Torus torus(2.0, 0.5);
  const torirender::Vec3 point(2.5, 0.0, 0.0);

  const torirender::Vec3 normal = torus.normal(point);

  REQUIRE_THAT(normal.x, WithinAbs(1.0, 1e-9));
  REQUIRE_THAT(normal.y, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(normal.z, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(normal.length(), WithinAbs(1.0, 1e-9));
}
