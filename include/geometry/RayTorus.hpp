#pragma once

#include <limits>

#include "core/Hit.hpp"
#include "core/Ray.hpp"
#include "geometry/Torus.hpp"

namespace torirender {

bool intersectRayTorus(const Ray& ray,
                       const Torus& torus,
                       HitRecord& hitRecord,
                       double tMin = 1e-4,
                       double tMax = std::numeric_limits<double>::infinity(),
                       bool useSdfFallback = true) noexcept;

}
