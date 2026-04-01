#include "geometry/BVH.hpp"

namespace torirender {

// construct node with 2 tori
BVHNode::BVHNode(const Torus& first, const Torus& second) noexcept : primitives_{first, second} {
  // bounding box enclosing both tori
  bounds_ = AABB::surroundingBox(primitives_[0].bounds(), primitives_[1].bounds());
}

// get first torus
const Torus& BVHNode::first() const noexcept {
  return primitives_[0];
}

// get second torus
const Torus& BVHNode::second() const noexcept {
  return primitives_[1];
}

// get both primitives
const std::array<Torus, 2>& BVHNode::primitives() const noexcept {
  return primitives_;
}

// get bounding box
const AABB& BVHNode::bounds() const noexcept {
  return bounds_;
}

}  // namespace torirender