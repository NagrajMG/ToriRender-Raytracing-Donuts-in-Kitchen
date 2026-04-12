#pragma once

#include <array>
#include <limits>

#include "core/Accel.hpp"
#include "core/Hit.hpp"
#include "core/Ray.hpp"
#include "geometry/AABB.hpp"
#include "geometry/Torus.hpp"

namespace torirender {

class BVHNode {
 public:
  BVHNode(const Torus& first, const Torus& second) noexcept;

  const Torus& first() const noexcept;
  const Torus& second() const noexcept;

  const std::array<Torus, 2>& primitives() const noexcept;
  const AABB& bounds() const noexcept;

  TORIRENDER_ACC_ROUTINE_SEQ
  bool hit(const Ray& ray,
           HitRecord& hitRecord,
           double tMin = 1e-4,  // self hit (to be ignored)
           double tMax = std::numeric_limits<double>::infinity())
      const noexcept;  // chose then object which was hit first

 private:
  std::array<Torus, 2> primitives_;
  std::array<AABB, 2> primitiveBounds_;
  AABB bounds_;
};

}  // namespace torirender
