// vector arithmatics
#include "math/Vec3.hpp"

#include <cassert>
#include <cmath>

namespace torirender {

double Vec3::lengthSquared() const noexcept {
  return (x * x) + (y * y) + (z * z);
}

double Vec3::length() const noexcept {
  return std::sqrt(lengthSquared());
}

Vec3 Vec3::normalized(double epsilon) const noexcept {
  const double len = length();
  if (len <= epsilon) {
    return Vec3{};
  }
  return *this / len;
}

void Vec3::normalize(double epsilon) noexcept {
  *this = normalized(epsilon);
}

double Vec3::dot(const Vec3& other) const noexcept {
  return (x * other.x) + (y * other.y) + (z * other.z);
}

Vec3 Vec3::cross(const Vec3& other) const noexcept {
  return Vec3(
      (y * other.z) - (z * other.y), (z * other.x) - (x * other.z), (x * other.y) - (y * other.x));
}

bool Vec3::nearZero(double epsilon) const noexcept {
  return std::fabs(x) <= epsilon && std::fabs(y) <= epsilon && std::fabs(z) <= epsilon;
}

double& Vec3::operator[](std::size_t index) noexcept {
  assert(index < 3U);
  if (index == 0U) {
    return x;
  }
  if (index == 1U) {
    return y;
  }
  return z;
}

const double& Vec3::operator[](std::size_t index) const noexcept {
  assert(index < 3U);
  if (index == 0U) {
    return x;
  }
  if (index == 1U) {
    return y;
  }
  return z;
}

Vec3& Vec3::operator+=(const Vec3& rhs) noexcept {
  x += rhs.x;
  y += rhs.y;
  z += rhs.z;
  return *this;
}

Vec3& Vec3::operator-=(const Vec3& rhs) noexcept {
  x -= rhs.x;
  y -= rhs.y;
  z -= rhs.z;
  return *this;
}

Vec3& Vec3::operator*=(double scalar) noexcept {
  x *= scalar;
  y *= scalar;
  z *= scalar;
  return *this;
}

Vec3& Vec3::operator/=(double scalar) noexcept {
  x /= scalar;
  y /= scalar;
  z /= scalar;
  return *this;
}

Vec3 operator+(Vec3 lhs, const Vec3& rhs) noexcept {
  lhs += rhs;
  return lhs;
}

Vec3 operator-(Vec3 lhs, const Vec3& rhs) noexcept {
  lhs -= rhs;
  return lhs;
}

Vec3 operator-(const Vec3& value) noexcept {
  return Vec3(-value.x, -value.y, -value.z);
}

Vec3 operator*(Vec3 value, double scalar) noexcept {
  value *= scalar;
  return value;
}

Vec3 operator*(double scalar, Vec3 value) noexcept {
  value *= scalar;
  return value;
}

Vec3 operator/(Vec3 value, double scalar) noexcept {
  value /= scalar;
  return value;
}

bool operator==(const Vec3& lhs, const Vec3& rhs) noexcept {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool operator!=(const Vec3& lhs, const Vec3& rhs) noexcept {
  return !(lhs == rhs);
}

double dot(const Vec3& lhs, const Vec3& rhs) noexcept {
  return lhs.dot(rhs);
}

Vec3 cross(const Vec3& lhs, const Vec3& rhs) noexcept {
  return lhs.cross(rhs);
}

}  // namespace torirender
