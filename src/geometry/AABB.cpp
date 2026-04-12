#include "geometry/AABB.hpp"

#include <algorithm>
#include <limits>

namespace torirender {

// empty box
AABB::AABB() noexcept
    : min_(std::numeric_limits<double>::infinity(),
           std::numeric_limits<double>::infinity(),
           std::numeric_limits<double>::infinity()),
      max_(-std::numeric_limits<double>::infinity(),
           -std::numeric_limits<double>::infinity(),
           -std::numeric_limits<double>::infinity()) {
}

AABB::AABB(const Vec3& minimum, const Vec3& maximum) noexcept : min_(minimum), max_(maximum) {
}  // good init

TORIRENDER_ACC_ROUTINE_SEQ
const Vec3& AABB::min() const noexcept {
  return min_;
}  // lower corner
TORIRENDER_ACC_ROUTINE_SEQ
const Vec3& AABB::max() const noexcept {
  return max_;
}  // upper corner

bool AABB::isValid() const noexcept {
  return min_.x <= max_.x && min_.y <= max_.y && min_.z <= max_.z;  // valid bounds
}

// include a point
void AABB::expand(const Vec3& point) noexcept {
  min_.x = std::min(min_.x, point.x);
  min_.y = std::min(min_.y, point.y);
  min_.z = std::min(min_.z, point.z);

  max_.x = std::max(max_.x, point.x);
  max_.y = std::max(max_.y, point.y);
  max_.z = std::max(max_.z, point.z);
}

// include another box
void AABB::expand(const AABB& other) noexcept {
  if (!other.isValid())
    return;
  expand(other.min());
  expand(other.max());
}

// check if point inside
bool AABB::contains(const Vec3& point, double epsilon) const noexcept {
  return point.x >= (min_.x - epsilon) && point.x <= (max_.x + epsilon) &&
         point.y >= (min_.y - epsilon) && point.y <= (max_.y + epsilon) &&
         point.z >= (min_.z - epsilon) && point.z <= (max_.z + epsilon);
}

// check if box inside
bool AABB::contains(const AABB& other, double epsilon) const noexcept {
  if (!other.isValid())
    return false;
  return contains(other.min(), epsilon) && contains(other.max(), epsilon);
}

// merge two boxes
AABB AABB::surroundingBox(const AABB& first, const AABB& second) noexcept {
  if (!first.isValid())
    return second;
  if (!second.isValid())
    return first;

  const Vec3 minimum(std::min(first.min().x, second.min().x),
                     std::min(first.min().y, second.min().y),
                     std::min(first.min().z, second.min().z));

  const Vec3 maximum(std::max(first.max().x, second.max().x),
                     std::max(first.max().y, second.max().y),
                     std::max(first.max().z, second.max().z));

  return AABB(minimum, maximum);
}

}  // namespace torirender
