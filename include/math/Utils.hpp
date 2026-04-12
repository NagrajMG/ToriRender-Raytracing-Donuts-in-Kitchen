#pragma once

#include <algorithm>
#include <cmath>

#include "core/Accel.hpp"

namespace torirender::math {

constexpr double kEpsilon = 1e-9;

TORIRENDER_ACC_ROUTINE_SEQ
inline double clamp(double value, double low, double high) noexcept {
  return std::max(low, std::min(value, high));
}

TORIRENDER_ACC_ROUTINE_SEQ
inline bool nearlyEqual(double a, double b, double epsilon = kEpsilon) noexcept {
  return std::fabs(a - b) <= epsilon;
}

TORIRENDER_ACC_ROUTINE_SEQ
inline double degreesToRadians(double degrees) noexcept {
  return degrees * 0.01745329251994329576923690768489;
}

}  // namespace torirender::math
