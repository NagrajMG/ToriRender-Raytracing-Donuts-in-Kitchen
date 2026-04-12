#pragma once

#include "core/Accel.hpp"
#include "math/Utils.hpp"
#include "math/Vec3.hpp"

namespace torirender {

class AABB {
 public:
  AABB() noexcept;  // default box

  AABB(const Vec3& minimum, const Vec3& maximum) noexcept;  // init bounds

  TORIRENDER_ACC_ROUTINE_SEQ
  const Vec3& min() const noexcept;  // lower corner
  TORIRENDER_ACC_ROUTINE_SEQ
  const Vec3& max() const noexcept;  // upper corner

  bool isValid() const noexcept;  // min <= max

  void expand(const Vec3& point) noexcept;  // include point
  void expand(const AABB& other) noexcept;  // include box

  bool contains(const Vec3& point, double epsilon = math::kEpsilon) const noexcept;  // point inside
  bool contains(const AABB& other,
                double epsilon = math::kEpsilon) const noexcept;  // box inside

  static AABB surroundingBox(const AABB& first,
                             const AABB& second) noexcept;  // merge boxes

 private:
  Vec3 min_;  // min corner
  Vec3 max_;  // max corner
};

}  // namespace torirender
