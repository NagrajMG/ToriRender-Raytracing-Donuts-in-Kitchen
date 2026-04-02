#include "geometry/Torus.hpp"

#include <algorithm>
#include <cmath>

namespace torirender {

namespace {

// ensure radii are positive and non-zero
double absoluteRadius(double value) noexcept {
  return std::max(std::fabs(value), math::kEpsilon);
}

// convert enum axis to corresponding axis vector
Vec3 axisFromEnum(TorusAxis axis) noexcept {
  switch (axis) {
    case TorusAxis::X:
      return Vec3(1.0, 0.0, 0.0);
    case TorusAxis::Y:
      return Vec3(0.0, 1.0, 0.0);
    case TorusAxis::Z:
      return Vec3(0.0, 0.0, 1.0);
  }
  return Vec3(0.0, 1.0, 0.0);  // defaulter is Y
}

// normalize axis, fallback if invalid
Vec3 normalizedAxis(const Vec3& axisDirection) noexcept {
  if (axisDirection.nearZero()) {
    return Vec3(0.0, 1.0, 0.0);
  }
  return axisDirection.normalized();
}

// map arbitrary direction to closest principal axis
TorusAxis closestPrincipalAxis(const Vec3& direction) noexcept {
  const Vec3 absDir(std::fabs(direction.x), std::fabs(direction.y), std::fabs(direction.z));
  if (absDir.x >= absDir.y && absDir.x >= absDir.z)
    return TorusAxis::X;
  if (absDir.z >= absDir.x && absDir.z >= absDir.y)
    return TorusAxis::Z;
  return TorusAxis::Y;
}

}  // namespace

// convert classification enum to string
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

// constructor using enum axis
Torus::Torus(double majorRadius, double minorRadius, const Vec3& center, TorusAxis axis) noexcept
    : majorRadius_(absoluteRadius(majorRadius)),
      minorRadius_(absoluteRadius(minorRadius)),
      center_(center),
      axis_(axis),
      axisDirection_(axisFromEnum(axis)) {
}

// constructor using arbitrary axis direction
Torus::Torus(double majorRadius,
             double minorRadius,
             const Vec3& center,
             const Vec3& axisDirection) noexcept
    : majorRadius_(absoluteRadius(majorRadius)),
      minorRadius_(absoluteRadius(minorRadius)),
      center_(center),
      axis_(TorusAxis::Y),
      axisDirection_(normalizedAxis(axisDirection)) {
  axis_ = closestPrincipalAxis(axisDirection_);
}

// getters
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
const Vec3& Torus::axisDirection() const noexcept {
  return axisDirection_;
}

// squared distance from axis (used in torus equation)
double Torus::perpendicularSquared(const Vec3& localPoint) const noexcept {
  const double radialSquared = localPoint.lengthSquared();
  const double axial = dot(localPoint, axisDirection_);
  return std::max(0.0, radialSquared - (axial * axial));
}

// implicit torus function F(p)
double Torus::evaluate(const Vec3& point) const noexcept {
  const Vec3 localPoint = point - center_;
  const double s =
      localPoint.lengthSquared() + (majorRadius_ * majorRadius_) - (minorRadius_ * minorRadius_);
  return (s * s) - (4.0 * majorRadius_ * majorRadius_ * perpendicularSquared(localPoint));
}

// classify point using implicit function
PointClassification Torus::classify(const Vec3& point, double epsilon) const noexcept {
  const double value = evaluate(point);
  if (value > epsilon)
    return PointClassification::Outside;
  if (value < -epsilon)
    return PointClassification::Inside;
  return PointClassification::On;
}

// gradient of implicit function (used for normal)
Vec3 Torus::gradient(const Vec3& point) const noexcept {
  const Vec3 localPoint = point - center_;
  const double radiusSquared = majorRadius_ * majorRadius_;
  const double s = localPoint.lengthSquared() + radiusSquared - (minorRadius_ * minorRadius_);

  const double axial = dot(localPoint, axisDirection_);
  const Vec3 radialComponent = localPoint - (axial * axisDirection_);

  return (4.0 * s * localPoint) - (8.0 * radiusSquared * radialComponent);
}

// normalized surface normal
Vec3 Torus::normal(const Vec3& point, double epsilon) const noexcept {
  return gradient(point).normalized(epsilon);
}

// bounding box of torus
AABB Torus::bounds() const noexcept {
  const Vec3& a = axisDirection_;

  const double extentX =
      (majorRadius_ * std::sqrt(std::max(0.0, 1.0 - (a.x * a.x)))) + minorRadius_;
  const double extentY =
      (majorRadius_ * std::sqrt(std::max(0.0, 1.0 - (a.y * a.y)))) + minorRadius_;
  const double extentZ =
      (majorRadius_ * std::sqrt(std::max(0.0, 1.0 - (a.z * a.z)))) + minorRadius_;

  const Vec3 extent(extentX, extentY, extentZ);

  return AABB(center_ - extent, center_ + extent);
}

}  // namespace torirender