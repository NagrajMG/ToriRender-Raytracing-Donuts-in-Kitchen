#include "geometry/RayMarcher.hpp"

#include <algorithm>
#include <cmath>

namespace torirender {

double torusSignedDistance(const Torus& torus, const Vec3& point) noexcept {
  const Vec3 local = point - torus.center();
  const Vec3 axis = torus.axisDirection();

  const double axial = dot(local, axis);
  const Vec3 radialVector = local - (axial * axis);
  const double radial = radialVector.length();

  const double q = radial - torus.majorRadius();
  return std::sqrt((q * q) + (axial * axial)) - torus.minorRadius();
}

bool marchRayToTorus(const Ray& ray,
                     const Torus& torus,
                     HitRecord& hitRecord,
                     double tMin,
                     double tMax,
                     int maxSteps,
                     double epsilon) noexcept {
  double t = std::max(tMin, 0.0);

  for (int step = 0; step < maxSteps && t <= tMax; ++step) {
    const Vec3 point = ray.at(t);
    const double distance = torusSignedDistance(torus, point);

    if (std::fabs(distance) <= epsilon) {
      hitRecord.t = t;
      hitRecord.point = point;
      const Vec3 outwardNormal = torus.normal(point, epsilon);
      hitRecord.setFaceNormal(ray, outwardNormal);
      return true;
    }

    const double stepLength = std::max(std::fabs(distance), epsilon * 0.5);
    t += stepLength;
  }

  return false;
}

}  // namespace torirender
