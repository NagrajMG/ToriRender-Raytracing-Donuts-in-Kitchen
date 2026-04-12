#pragma once

#include "core/Accel.hpp"
#include "math/Vec3.hpp"

namespace torirender {

class Ray {
 public:
  Ray() noexcept;  // default ray

  Ray(const Vec3& origin, const Vec3& direction) noexcept;  // init

  TORIRENDER_ACC_ROUTINE_SEQ
  const Vec3& origin() const noexcept;  // start point
  TORIRENDER_ACC_ROUTINE_SEQ
  const Vec3& direction() const noexcept;  // direction

  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3 at(double t) const noexcept;  // origin + t * direction (basically return
                                     // a point on that ray)

 private:
  Vec3 origin_;     // origin
  Vec3 direction_;  // direction
};

}  // namespace torirender
