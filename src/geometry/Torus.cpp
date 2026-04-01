#include "geometry/Torus.hpp"

#include <algorithm>
#include <cmath>

namespace torirender {

namespace {

// ensure radius is positive and non-zero
double absoluteRadius(double value) noexcept {
  return std::max(std::fabs(value), math::kEpsilon);
}

}  // namespace

const char* toString(PointClassification classification) noexcept {
  switch (classification) {
    case PointClassification::Inside:
      return "INSIDE";
    case PointClassification::On:
      return "ON";
    case PointClassification::Outside:
      return "OUTSIDE";
  }
  return "UNKNOWN";
}

// construct torus
Torus::Torus(double majorRadius, double minorRadius, const Vec3& center, TorusAxis axis) noexcept
    : majorRadius_(absoluteRadius(majorRadius)),
      minorRadius_(absoluteRadius(minorRadius)),
      center_(center),
      axis_(axis) {
}

double Torus::majorRadius() const noexcept {
  return majorRadius_;
}
double Torus::minorRadius() const noexcept {
  return minorRadius_;
}
const Vec3& Torus::center() const noexcept {
  return center_;
}
TorusAxis Torus::axis() const noexcept {
  return axis_;
}

// distance squared from axis plane
double Torus::perpendicularSquared(const Vec3& localPoint) const noexcept {
  switch (axis_) {
    case TorusAxis::X:
      return localPoint.y * localPoint.y + localPoint.z * localPoint.z;
    case TorusAxis::Y:
      return localPoint.x * localPoint.x + localPoint.z * localPoint.z;
    case TorusAxis::Z:
      return localPoint.x * localPoint.x + localPoint.y * localPoint.y;
  }
  return 0.0;
}

// implicit torus equation function value if i put the point
double Torus::evaluate(const Vec3& point) const noexcept {
  const Vec3 localPoint = point - center_;

  const double x2 = localPoint.x * localPoint.x;
  const double y2 = localPoint.y * localPoint.y;
  const double z2 = localPoint.z * localPoint.z;

  const double s = x2 + y2 + z2 + majorRadius_ * majorRadius_ - minorRadius_ * minorRadius_;
  return (s * s) - (4.0 * majorRadius_ * majorRadius_ * perpendicularSquared(localPoint));
}

// inside / on / outside
PointClassification Torus::classify(const Vec3& point, double epsilon) const noexcept {
  const double value = evaluate(point);

  if (value > epsilon)
    return PointClassification::Outside;
  if (value < -epsilon)
    return PointClassification::Inside;
  return PointClassification::On;
}

// gradient of implicit function
Vec3 Torus::gradient(const Vec3& point) const noexcept {
  const Vec3 localPoint = point - center_;
  const double x = localPoint.x;
  const double y = localPoint.y;
  const double z = localPoint.z;

  const double s =
      localPoint.lengthSquared() + majorRadius_ * majorRadius_ - minorRadius_ * minorRadius_;
  const double common = s - (2.0 * majorRadius_ * majorRadius_);

  switch (axis_) {
    case TorusAxis::X:
      return Vec3(4 * x * s, 4 * y * common, 4 * z * common);
    case TorusAxis::Y:
      return Vec3(4 * x * common, 4 * y * s, 4 * z * common);
    case TorusAxis::Z:
      return Vec3(4 * x * common, 4 * y * common, 4 * z * s);
  }
  return Vec3{};
}

// unit normal
Vec3 Torus::normal(const Vec3& point, double epsilon) const noexcept {
  return gradient(point).normalized(epsilon);
}

// bounding box
AABB Torus::bounds() const noexcept {
  const double majorExtent = majorRadius_ + minorRadius_;

  Vec3 extent;
  switch (axis_) {
    case TorusAxis::X:
      extent = Vec3(minorRadius_, majorExtent, majorExtent);
      break;
    case TorusAxis::Y:
      extent = Vec3(majorExtent, minorRadius_, majorExtent);
      break;
    case TorusAxis::Z:
      extent = Vec3(majorExtent, majorExtent, minorRadius_);
      break;
  }

  return AABB(center_ - extent, center_ + extent);
}

}  // namespace torirender