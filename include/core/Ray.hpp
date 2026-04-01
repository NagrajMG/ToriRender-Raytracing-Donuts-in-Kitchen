#pragma once

#include "math/Vec3.hpp"

namespace torirender {

class Ray {
 public:
  Ray() noexcept;  // default ray

  Ray(const Vec3& origin, const Vec3& direction) noexcept;  // init

  const Vec3& origin() const noexcept;     // start point
  const Vec3& direction() const noexcept;  // direction

  Vec3 at(double t) const noexcept;  // origin + t * direction (basically return
                                     // a point on that ray)

 private:
  Vec3 origin_;     // origin
  Vec3 direction_;  // direction
};

}  // namespace torirender