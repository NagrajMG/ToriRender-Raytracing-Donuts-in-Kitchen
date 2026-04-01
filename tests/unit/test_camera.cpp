#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Camera.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("Camera center ray points at lookAt", "[camera]") {
  const torirender::Vec3 lookFrom(1.0, 2.0, 3.0);
  const torirender::Vec3 lookAt(-2.0, 4.0, 1.0);
  const torirender::Camera camera(
      lookFrom, lookAt, torirender::Vec3(0.0, 1.0, 0.0), 45.0, 1.0, 101, 101);

  const torirender::Ray centerRay = camera.getRay(50, 50);
  const torirender::Vec3 expected = (lookAt - lookFrom).normalized();

  REQUIRE_THAT(centerRay.direction().x, WithinAbs(expected.x, 1e-9));
  REQUIRE_THAT(centerRay.direction().y, WithinAbs(expected.y, 1e-9));
  REQUIRE_THAT(centerRay.direction().z, WithinAbs(expected.z, 1e-9));
}

TEST_CASE("Camera remains stable when viewUp is parallel to view direction", "[camera]") {
  const torirender::Camera camera(torirender::Vec3(0.0, 0.0, 0.0),
                                  torirender::Vec3(0.0, 1.0, 0.0),
                                  torirender::Vec3(0.0, 1.0, 0.0),
                                  60.0,
                                  1.0,
                                  101,
                                  101);

  const torirender::Ray centerRay = camera.getRay(50, 50);

  REQUIRE_THAT(centerRay.direction().x, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(centerRay.direction().y, WithinAbs(1.0, 1e-9));
  REQUIRE_THAT(centerRay.direction().z, WithinAbs(0.0, 1e-9));
}

TEST_CASE("Camera image dimensions fall back from aspect ratio", "[camera]") {
  const torirender::Camera camera(torirender::Vec3(0.0, 0.0, 0.0),
                                  torirender::Vec3(0.0, 0.0, -1.0),
                                  torirender::Vec3(0.0, 1.0, 0.0),
                                  60.0,
                                  16.0 / 9.0,
                                  320,
                                  -1);

  REQUIRE(camera.imageWidth() == 320);
  REQUIRE(camera.imageHeight() == 180);
}
