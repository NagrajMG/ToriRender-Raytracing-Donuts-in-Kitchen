#pragma once

#include "core/Accel.hpp"
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

const char* toString(PointClassification classification) noexcept;  // enum to string

class Torus {
 public:
  Torus(double majorRadius,
        double minorRadius,
        const Vec3& center = Vec3{},
        TorusAxis axis = TorusAxis::Y) noexcept;  // define torus

  Torus(double majorRadius,
        double minorRadius,
        const Vec3& center,
        const Vec3& axisDirection) noexcept;  // custom axis

  // Getters
  TORIRENDER_ACC_ROUTINE_SEQ
  double majorRadius() const noexcept;  // R
  TORIRENDER_ACC_ROUTINE_SEQ
  double minorRadius() const noexcept;  // r
  TORIRENDER_ACC_ROUTINE_SEQ
  const Vec3& center() const noexcept;  // position
  TORIRENDER_ACC_ROUTINE_SEQ
  TorusAxis axis() const noexcept;  // orientation
  TORIRENDER_ACC_ROUTINE_SEQ
  const Vec3& axisDirection() const noexcept;

  TORIRENDER_ACC_ROUTINE_SEQ
  double evaluate(const Vec3& point) const noexcept;  // implicit equation value

  PointClassification classify(
      const Vec3& point,
      double epsilon = math::kEpsilon) const noexcept;  // inside/on/outside

  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3 gradient(
      const Vec3& point) const noexcept;  // points in the direction of the maximum rate of increase
                                          // of the function and is perpendicular to normal

  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3 normal(const Vec3& point,
              double epsilon = math::kEpsilon) const noexcept;  // unit normal

  AABB bounds() const noexcept;  // get the bounding box

 private:
  TORIRENDER_ACC_ROUTINE_SEQ
  double perpendicularSquared(const Vec3& localPoint) const noexcept;  // squared distance from axis

  double majorRadius_;  // R
  double minorRadius_;  // r
  Vec3 center_;         // center
  TorusAxis axis_;      // axis
  Vec3 axisDirection_;
};

}  // namespace torirender
