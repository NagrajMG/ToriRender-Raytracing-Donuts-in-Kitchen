#pragma once

#include <cstddef>

namespace torirender {

class Vec3 {
 public:
  double x, y, z;  // components

  constexpr Vec3() noexcept : x(0.0), y(0.0), z(0.0) {
  }  // zero vector
  constexpr Vec3(double xValue, double yValue, double zValue) noexcept
      : x(xValue), y(yValue), z(zValue) {
  }  // init

  double lengthSquared() const noexcept;  // x^2 + y^2 + z^2
  double length() const noexcept;         // magnitude

  Vec3 normalized(double epsilon = 1e-12) const noexcept;  // return unit vector
  void normalize(double epsilon = 1e-12) noexcept;         // in-place normalize

  double dot(const Vec3& other) const noexcept;  // dot product
  Vec3 cross(const Vec3& other) const noexcept;  // cross product

  bool nearZero(double epsilon = 1e-12) const noexcept;  // ~zero check

  double& operator[](std::size_t index) noexcept;  // access x,y,z
  const double& operator[](std::size_t index) const noexcept;

  Vec3& operator+=(const Vec3& rhs) noexcept;  // add in-place
  Vec3& operator-=(const Vec3& rhs) noexcept;  // subtract in-place
  Vec3& operator*=(double scalar) noexcept;    // scale in-place
  Vec3& operator/=(double scalar) noexcept;    // divide in-place
};

// basic arithmetic
Vec3 operator+(Vec3 lhs, const Vec3& rhs) noexcept;
Vec3 operator-(Vec3 lhs, const Vec3& rhs) noexcept;
Vec3 operator-(const Vec3& value) noexcept;

// scalar ops
Vec3 operator*(Vec3 value, double scalar) noexcept;
Vec3 operator*(double scalar, Vec3 value) noexcept;
Vec3 operator/(Vec3 value, double scalar) noexcept;

// comparisons
bool operator==(const Vec3& lhs, const Vec3& rhs) noexcept;
bool operator!=(const Vec3& lhs, const Vec3& rhs) noexcept;

// vector functions
double dot(const Vec3& lhs, const Vec3& rhs) noexcept;
Vec3 cross(const Vec3& lhs, const Vec3& rhs) noexcept;

}  // namespace torirender