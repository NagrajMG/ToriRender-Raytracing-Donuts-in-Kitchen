#include "core/Ray.hpp"

namespace torirender {

Ray::Ray() noexcept : origin_(0.0, 0.0, 0.0), direction_(0.0, 0.0, -1.0) {
}

Ray::Ray(const Vec3& origin, const Vec3& direction) noexcept
    : origin_(origin), direction_(direction) {
}

const Vec3& Ray::origin() const noexcept {
  return origin_;
}

const Vec3& Ray::direction() const noexcept {
  return direction_;
}

Vec3 Ray::at(double t) const noexcept {
  return origin_ + (direction_ * t);
}

}  // namespace torirender
