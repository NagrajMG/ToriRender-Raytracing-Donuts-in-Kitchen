#include "geometry/BVH.hpp"

#include <algorithm>
#include <cmath>

#include "geometry/RayTorus.hpp"

namespace torirender {

namespace {

bool hitAabb(const AABB& box, const Ray& ray, double tMin, double tMax) noexcept {
  for (int axis = 0; axis < 3; ++axis) {
    const double origin = ray.origin()[static_cast<std::size_t>(axis)];
    const double direction = ray.direction()[static_cast<std::size_t>(axis)];
    const double minBound = box.min()[static_cast<std::size_t>(axis)];
    const double maxBound = box.max()[static_cast<std::size_t>(axis)];

    if (std::fabs(direction) <= 1e-12) {
      if (origin < minBound || origin > maxBound) {
        return false;
      }
      continue;
    }

    const double inverseDirection = 1.0 / direction;
    double t0 = (minBound - origin) * inverseDirection;
    double t1 = (maxBound - origin) * inverseDirection;
    if (t0 > t1) {
      std::swap(t0, t1);
    }

    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMax < tMin) {
      return false;
    }
  }

  return true;
}

}  // namespace

BVHNode::BVHNode(const Torus& first, const Torus& second) noexcept
    : primitives_{first, second}, primitiveBounds_{first.bounds(), second.bounds()} {
  bounds_ = AABB::surroundingBox(primitiveBounds_[0], primitiveBounds_[1]);
}

const Torus& BVHNode::first() const noexcept {
  return primitives_[0];
}

const Torus& BVHNode::second() const noexcept {
  return primitives_[1];
}

const std::array<Torus, 2>& BVHNode::primitives() const noexcept {
  return primitives_;
}

const AABB& BVHNode::bounds() const noexcept {
  return bounds_;
}

bool BVHNode::hit(const Ray& ray, HitRecord& hitRecord, double tMin, double tMax) const noexcept {
  if (!hitAabb(bounds_, ray, tMin, tMax)) {
    return false;
  }

  bool foundHit = false;
  double closestT = tMax;
  HitRecord candidate{};

  for (std::size_t i = 0; i < primitives_.size(); ++i) {
    if (!hitAabb(primitiveBounds_[i], ray, tMin, closestT)) {
      continue;
    }

    candidate.primitiveId = static_cast<int>(i);
    if (intersectRayTorus(ray, primitives_[i], candidate, tMin, closestT, false)) {
      foundHit = true;
      closestT = candidate.t;
      hitRecord = candidate;
    }
  }

  return foundHit;
}

}  // namespace torirender
