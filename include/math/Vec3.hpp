#pragma once

#include <cstddef>

#include "core/Accel.hpp"

namespace torirender {

class Vec3 {
 public:
  double x, y, z;  // components

  constexpr Vec3() noexcept : x(0.0), y(0.0), z(0.0) {
  }  // zero vector
  constexpr Vec3(double xValue, double yValue, double zValue) noexcept
      : x(xValue), y(yValue), z(zValue) {
  }  // init

  TORIRENDER_ACC_ROUTINE_SEQ
  double lengthSquared() const noexcept;  // x^2 + y^2 + z^2
  TORIRENDER_ACC_ROUTINE_SEQ
  double length() const noexcept;  // magnitude

  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3 normalized(double epsilon = 1e-12) const noexcept;  // return unit vector
  TORIRENDER_ACC_ROUTINE_SEQ
  void normalize(double epsilon = 1e-12) noexcept;  // in-place normalize

  TORIRENDER_ACC_ROUTINE_SEQ
  double dot(const Vec3& other) const noexcept;  // dot product
  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3 cross(const Vec3& other) const noexcept;  // cross product

  TORIRENDER_ACC_ROUTINE_SEQ
  bool nearZero(double epsilon = 1e-12) const noexcept;  // ~zero check

  TORIRENDER_ACC_ROUTINE_SEQ
  double& operator[](std::size_t index) noexcept;  // access x,y,z
  TORIRENDER_ACC_ROUTINE_SEQ
  const double& operator[](std::size_t index) const noexcept;

  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3& operator+=(const Vec3& rhs) noexcept;  // add in-place
  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3& operator-=(const Vec3& rhs) noexcept;  // subtract in-place
  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3& operator*=(double scalar) noexcept;  // scale in-place
  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3& operator/=(double scalar) noexcept;  // divide in-place
};

// basic arithmetic
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 operator+(Vec3 lhs, const Vec3& rhs) noexcept;
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 operator-(Vec3 lhs, const Vec3& rhs) noexcept;
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 operator-(const Vec3& value) noexcept;

// scalar ops
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 operator*(Vec3 value, double scalar) noexcept;
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 operator*(double scalar, Vec3 value) noexcept;
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 operator/(Vec3 value, double scalar) noexcept;

// comparisons
TORIRENDER_ACC_ROUTINE_SEQ
bool operator==(const Vec3& lhs, const Vec3& rhs) noexcept;
TORIRENDER_ACC_ROUTINE_SEQ
bool operator!=(const Vec3& lhs, const Vec3& rhs) noexcept;

// vector functions
TORIRENDER_ACC_ROUTINE_SEQ
double dot(const Vec3& lhs, const Vec3& rhs) noexcept;
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 cross(const Vec3& lhs, const Vec3& rhs) noexcept;

}  // namespace torirender
