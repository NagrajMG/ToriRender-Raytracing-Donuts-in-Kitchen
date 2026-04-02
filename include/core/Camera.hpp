#pragma once

#include "core/Ray.hpp"

namespace torirender {

class Camera {
 public:
  Camera() noexcept;

  Camera(const Vec3& lookFrom,
         const Vec3& lookAt,
         const Vec3& viewUp,
         double verticalFovDegrees,
         double aspectRatio,
         int imageWidth = 400,
         int imageHeight = -1) noexcept;

  int imageWidth() const noexcept;
  int imageHeight() const noexcept;

  // Sub-pixel ray for jittered sampling or anti-aliasing (monte carlo)
  Ray getRay(double pixelX, double pixelY) const noexcept;
  // Pixel-center ray for deterministic single-sample rendering
  Ray getRay(int pixelX, int pixelY) const noexcept;

 private:
  Vec3 origin_;
  Vec3 pixel00Loc_;
  Vec3 pixelDeltaU_;
  Vec3 pixelDeltaV_;
  Vec3 u_;
  Vec3 v_;
  Vec3 w_;
  int imageWidth_;
  int imageHeight_;
};

}  // namespace torirender
