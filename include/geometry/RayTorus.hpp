#pragma once

#include <limits>

#include "core/Accel.hpp"
#include "core/Hit.hpp"
#include "core/Ray.hpp"
#include "geometry/Torus.hpp"

namespace torirender {

TORIRENDER_ACC_ROUTINE_SEQ
bool intersectRayTorus(const Ray& ray,
                       const Torus& torus,
                       HitRecord& hitRecord,
                       double tMin = 1e-4,
                       double tMax = std::numeric_limits<double>::infinity(),
                       bool useSdfFallback = false) noexcept;

}
