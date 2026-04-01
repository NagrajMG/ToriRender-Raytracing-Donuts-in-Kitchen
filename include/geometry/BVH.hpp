#pragma once

#include <array>

#include "geometry/AABB.hpp"
#include "geometry/Torus.hpp"

namespace torirender {

class BVHNode {
 public:
  BVHNode(const Torus& first,
          const Torus& second) noexcept;  // build node with 2 tori

  const Torus& first() const noexcept;   // first primitive
  const Torus& second() const noexcept;  // second primitive

  const std::array<Torus, 2>& primitives() const noexcept;  // both primitives

  const AABB& bounds() const noexcept;  // enclosing box

 private:
  std::array<Torus, 2> primitives_;  // 2 tori
  AABB bounds_;                      // bounding box
};

}  // namespace torirender