#pragma once

#include <limits>

#include "core/Hit.hpp"
#include "core/Ray.hpp"
#include "geometry/Torus.hpp"

namespace torirender {

// signed distance from point to torus surface
double torusSignedDistance(const Torus& torus, const Vec3& point) noexcept;

// ray marching intersection with torus
bool marchRayToTorus(const Ray& ray,
                     const Torus& torus,
                     HitRecord& hitRecord,
                     double tMin = 1e-4,                                     // start distance
                     double tMax = std::numeric_limits<double>::infinity(),  // max distance
                     int maxSteps = 128,                                     // iteration limit
                     double epsilon = 1e-4) noexcept;                        // hit threshold

}  // namespace torirender