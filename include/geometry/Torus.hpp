#pragma once

#include "geometry/AABB.hpp"
#include "math/Utils.hpp"
#include "math/Vec3.hpp"

namespace torirender {

enum class TorusAxis {
  X,
  Y,
  Z  // orientation
};

enum class PointClassification { Inside, On, Outside };

const char* toString(PointClassification classification) noexcept;  // enum → string

class Torus {
 public:
  Torus(double majorRadius,
        double minorRadius,
        const Vec3& center = Vec3{},
        TorusAxis axis = TorusAxis::Y) noexcept;  // define torus

  // Getters
  double majorRadius() const noexcept;  // R
  double minorRadius() const noexcept;  // r
  const Vec3& center() const noexcept;  // position
  TorusAxis axis() const noexcept;      // orientation

  double evaluate(const Vec3& point) const noexcept;  // implicit equation value

  PointClassification classify(
      const Vec3& point,
      double epsilon = math::kEpsilon) const noexcept;  // inside/on/outside

  Vec3 gradient(
      const Vec3& point) const noexcept;  // points in the direction of the maximum rate of increase
                                          // of the function and is perpendicular to normal

  Vec3 normal(const Vec3& point,
              double epsilon = math::kEpsilon) const noexcept;  // unit normal

  AABB bounds() const noexcept;  // get the bounding box

 private:
  double perpendicularSquared(const Vec3& localPoint) const noexcept;  // squared distance from axis

  double majorRadius_;  // R
  double minorRadius_;  // r
  Vec3 center_;         // center
  TorusAxis axis_;      // axis
};

}  // namespace torirender