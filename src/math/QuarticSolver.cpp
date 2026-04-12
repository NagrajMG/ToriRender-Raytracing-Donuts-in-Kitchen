#include "math/QuarticSolver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

namespace torirender::math {

namespace {

constexpr int kMaxIterations = 256;
constexpr int kMaxDegree = 4;
constexpr std::size_t kMaxCollectedRoots = 32;

struct RealRootBuffer {
  std::array<double, kMaxCollectedRoots> values{};
  std::size_t count = 0;

  void push(double value) noexcept {
    if (count < values.size()) {
      values[count++] = value;
    }
  }
};

template <typename T>
T evaluatePolynomial(const std::array<double, 5>& coeffs, int degree, const T& x) noexcept {
  T result = static_cast<T>(coeffs[0]);
  for (int i = 1; i <= degree; ++i) {
    result = (result * x) + static_cast<T>(coeffs[static_cast<std::size_t>(i)]);
  }
  return result;
}

double evaluatePolynomialDerivative(const std::array<double, 5>& coeffs,
                                    int degree,
                                    double x) noexcept {
  if (degree <= 0) {
    return 0.0;
  }

  double result = coeffs[0] * static_cast<double>(degree);
  for (int i = 1; i < degree; ++i) {
    result = (result * x) + (coeffs[static_cast<std::size_t>(i)] * static_cast<double>(degree - i));
  }
  return result;
}

TORIRENDER_ACC_ROUTINE_SEQ
double sanitizeTolerance(double tolerance) noexcept {
  return tolerance > 0.0 ? tolerance : 1e-12;
}

bool compactCoefficients(const std::array<double, 5>& input,
                         double tolerance,
                         std::array<double, 5>& compact,
                         int& degree) noexcept {
  int first = 0;
  while (first < 5 && std::fabs(input[static_cast<std::size_t>(first)]) <= tolerance) {
    ++first;
  }

  if (first >= 5) {
    degree = -1;
    compact.fill(0.0);
    return false;
  }

  degree = 4 - first;
  compact.fill(0.0);
  for (int i = 0; i <= degree; ++i) {
    compact[static_cast<std::size_t>(i)] = input[static_cast<std::size_t>(first + i)];
  }
  return true;
}

RealRootBuffer solveLinear(double a, double b, double tolerance) noexcept {
  RealRootBuffer roots{};
  if (std::fabs(a) <= tolerance) {
    return roots;
  }
  roots.push(-b / a);
  return roots;
}

RealRootBuffer solveQuadratic(double a, double b, double c, double tolerance) noexcept {
  if (std::fabs(a) <= tolerance) {
    return solveLinear(b, c, tolerance);
  }

  RealRootBuffer roots{};
  const double discriminant = (b * b) - (4.0 * a * c);
  if (discriminant < -tolerance) {
    return roots;
  }
  if (std::fabs(discriminant) <= tolerance) {
    roots.push(-b / (2.0 * a));
    return roots;
  }

  const double sqrtDiscriminant = std::sqrt(discriminant);
  const double q = -0.5 * (b + std::copysign(sqrtDiscriminant, b));
  roots.push(q / a);
  if (std::fabs(q) > tolerance) {
    roots.push(c / q);
  }
  return roots;
}

std::array<std::complex<double>, kMaxDegree> durandKerner(const std::array<double, 5>& monicCoeffs,
                                                          int degree,
                                                          double tolerance) noexcept {
  std::array<std::complex<double>, kMaxDegree> roots{};
  std::array<std::complex<double>, kMaxDegree> nextRoots{};

  double bound = 1.0;
  for (int i = 1; i <= degree; ++i) {
    bound = std::max(bound, std::fabs(monicCoeffs[static_cast<std::size_t>(i)]));
  }
  const double radius = 1.0 + bound;

  constexpr double kTwoPi = 6.283185307179586476925286766559;
  for (int i = 0; i < degree; ++i) {
    const double angle = kTwoPi * static_cast<double>(i) / static_cast<double>(degree);
    roots[static_cast<std::size_t>(i)] = std::polar(radius, angle);
  }

  for (int iter = 0; iter < kMaxIterations; ++iter) {
    double maxDelta = 0.0;
    for (int i = 0; i < degree; ++i) {
      const std::complex<double> zi = roots[static_cast<std::size_t>(i)];
      std::complex<double> denom(1.0, 0.0);

      for (int j = 0; j < degree; ++j) {
        if (i == j) {
          continue;
        }
        denom *= (zi - roots[static_cast<std::size_t>(j)]);
      }

      if (std::abs(denom) <= tolerance) {
        denom = std::complex<double>(tolerance, tolerance);
      }

      const std::complex<double> delta = evaluatePolynomial(monicCoeffs, degree, zi) / denom;
      nextRoots[static_cast<std::size_t>(i)] = zi - delta;
      maxDelta = std::max(maxDelta, std::abs(delta));
    }

    roots = nextRoots;
    if (maxDelta <= tolerance) {
      break;
    }
  }

  return roots;
}

double refineRealRootNewton(const std::array<double, 5>& coeffs,
                            int degree,
                            double initial,
                            double tolerance) noexcept {
  double x = initial;
  for (int iter = 0; iter < 16; ++iter) {
    const double value = evaluatePolynomial(coeffs, degree, x);
    const double slope = evaluatePolynomialDerivative(coeffs, degree, x);
    if (std::fabs(slope) <= tolerance) {
      break;
    }

    const double next = x - (value / slope);
    if (!std::isfinite(next)) {
      break;
    }
    if (std::fabs(next - x) <= tolerance) {
      x = next;
      break;
    }
    x = next;
  }
  return x;
}

double rootBound(const std::array<double, 5>& monicCoeffs, int degree) noexcept {
  double maxCoeff = 0.0;
  for (int i = 1; i <= degree; ++i) {
    maxCoeff = std::max(maxCoeff, std::fabs(monicCoeffs[static_cast<std::size_t>(i)]));
  }
  return 1.0 + maxCoeff;
}

double bisectRoot(const std::array<double, 5>& coeffs,
                  int degree,
                  double left,
                  double right) noexcept {
  double fLeft = evaluatePolynomial(coeffs, degree, left);
  for (int iter = 0; iter < 80; ++iter) {
    const double mid = 0.5 * (left + right);
    const double fMid = evaluatePolynomial(coeffs, degree, mid);
    if (fMid == 0.0) {
      return mid;
    }

    if ((fLeft > 0.0 && fMid < 0.0) || (fLeft < 0.0 && fMid > 0.0)) {
      right = mid;
    } else {
      left = mid;
      fLeft = fMid;
    }
  }

  return 0.5 * (left + right);
}

bool collectRootsFromSampling(const std::array<double, 5>& coeffs,
                              int degree,
                              const std::array<double, 5>& monicCoeffs,
                              double tolerance,
                              int sampleCount,
                              RealRootBuffer& roots) noexcept {
  const double bound = rootBound(monicCoeffs, degree);
  const int clampedSamples = std::max(sampleCount, 16);
  const double step = (2.0 * bound) / static_cast<double>(clampedSamples);
  const double sampleTolerance = std::max(100.0 * tolerance, 1e-6);
  bool sawHint = false;

  double previousX = -bound;
  double previousF = evaluatePolynomial(coeffs, degree, previousX);

  for (int i = 1; i <= clampedSamples; ++i) {
    const double x = -bound + (static_cast<double>(i) * step);
    const double f = evaluatePolynomial(coeffs, degree, x);

    if (std::fabs(f) <= sampleTolerance) {
      sawHint = true;
      const double refined = refineRealRootNewton(coeffs, degree, x, tolerance);
      if (std::isfinite(refined) &&
          std::fabs(evaluatePolynomial(coeffs, degree, refined)) <= 1e-6) {
        roots.push(refined);
      }
    }

    if ((previousF > 0.0 && f < 0.0) || (previousF < 0.0 && f > 0.0)) {
      sawHint = true;
      const double bracketRoot = bisectRoot(coeffs, degree, previousX, x);
      const double refined = refineRealRootNewton(coeffs, degree, bracketRoot, tolerance);
      if (std::isfinite(refined) &&
          std::fabs(evaluatePolynomial(coeffs, degree, refined)) <= 1e-6) {
        roots.push(refined);
      }
    }

    previousX = x;
    previousF = f;
  }

  return sawHint;
}

void sortAndUniqueRoots(RealRootBuffer& roots, double tolerance) noexcept {
  std::sort(roots.values.begin(), roots.values.begin() + static_cast<std::ptrdiff_t>(roots.count));

  std::size_t write = 0;
  for (std::size_t i = 0; i < roots.count; ++i) {
    const double root = roots.values[i];
    if (write == 0 || std::fabs(root - roots.values[write - 1]) > (8.0 * tolerance)) {
      roots.values[write++] = root;
    }
  }
  roots.count = write;
}

std::vector<double> toVector(const RealRootBuffer& roots) {
  std::vector<double> out;
  out.reserve(roots.count);
  for (std::size_t i = 0; i < roots.count; ++i) {
    out.push_back(roots.values[i]);
  }
  return out;
}

RealRootBuffer solvePolynomialRealBuffer(const std::array<double, 5>& input, double tolerance) {
  const double tol = sanitizeTolerance(tolerance);

  std::array<double, 5> coeffs{};
  int degree = -1;
  if (!compactCoefficients(input, tol, coeffs, degree) || degree <= 0) {
    return RealRootBuffer{};
  }

  if (degree == 1) {
    return solveLinear(coeffs[0], coeffs[1], tol);
  }

  if (degree == 2) {
    return solveQuadratic(coeffs[0], coeffs[1], coeffs[2], tol);
  }

  const double leading = coeffs[0];
  if (std::fabs(leading) <= tol) {
    return RealRootBuffer{};
  }

  std::array<double, 5> monic = coeffs;
  for (int i = 0; i <= degree; ++i) {
    monic[static_cast<std::size_t>(i)] /= leading;
  }

  const auto complexRoots = durandKerner(monic, degree, tol);

  RealRootBuffer realRoots{};
  const double imagTolerance = std::sqrt(tol);
  double minImagAbs = std::numeric_limits<double>::infinity();

  for (int i = 0; i < degree; ++i) {
    const std::complex<double>& root = complexRoots[static_cast<std::size_t>(i)];
    minImagAbs = std::min(minImagAbs, std::fabs(root.imag()));

    if (std::fabs(root.imag()) > imagTolerance || !std::isfinite(root.real())) {
      continue;
    }

    const double refined = refineRealRootNewton(coeffs, degree, root.real(), tol);
    const double residual = std::fabs(evaluatePolynomial(coeffs, degree, refined));
    if (residual <= 1e-7) {
      realRoots.push(refined);
    }
  }

  sortAndUniqueRoots(realRoots, tol);

  if (realRoots.count == 0) {
    constexpr double kAmbiguousImagThreshold = 1e-3;
    const bool forceRecoverySearch = minImagAbs <= kAmbiguousImagThreshold;
    // Optimization worked out here: skip costly sampling search on clear "no real root" cases.
    if (!forceRecoverySearch) {
      return RealRootBuffer{};
    }
    constexpr int kSampleLevels[] = {128, 512, 2048};

    for (int sampleLevel : kSampleLevels) {
      const std::size_t before = realRoots.count;
      const bool sawHint =
          collectRootsFromSampling(coeffs, degree, monic, tol, sampleLevel, realRoots);

      sortAndUniqueRoots(realRoots, tol);

      if (static_cast<int>(realRoots.count) >= degree) {
        break;
      }
      if (!sawHint && !forceRecoverySearch) {
        break;
      }
      if (!sawHint && sampleLevel >= 512) {
        break;
      }
      if (realRoots.count == before && sampleLevel >= 512) {
        break;
      }
    }
  }

  sortAndUniqueRoots(realRoots, tol);
  return realRoots;
}

TORIRENDER_ACC_ROUTINE_SEQ
double evaluateQuarticDevice(
    double a4, double a3, double a2, double a1, double a0, double t) noexcept {
  return (((a4 * t + a3) * t + a2) * t + a1) * t + a0;
}

TORIRENDER_ACC_ROUTINE_SEQ
double evaluateQuarticDerivativeDevice(
    double a4, double a3, double a2, double a1, double t) noexcept {
  return ((4.0 * a4 * t + 3.0 * a3) * t + 2.0 * a2) * t + a1;
}

TORIRENDER_ACC_ROUTINE_SEQ
double bisectQuarticDevice(
    double a4, double a3, double a2, double a1, double a0, double left, double right) noexcept {
  double fLeft = evaluateQuarticDevice(a4, a3, a2, a1, a0, left);
  for (int iter = 0; iter < 80; ++iter) {
    const double mid = 0.5 * (left + right);
    const double fMid = evaluateQuarticDevice(a4, a3, a2, a1, a0, mid);
    if (fMid == 0.0) {
      return mid;
    }

    if ((fLeft > 0.0 && fMid < 0.0) || (fLeft < 0.0 && fMid > 0.0)) {
      right = mid;
    } else {
      left = mid;
      fLeft = fMid;
    }
  }

  return 0.5 * (left + right);
}

TORIRENDER_ACC_ROUTINE_SEQ
double refineQuarticNewtonDevice(double a4,
                                 double a3,
                                 double a2,
                                 double a1,
                                 double a0,
                                 double initial,
                                 double tolerance) noexcept {
  double x = initial;
  for (int iter = 0; iter < 16; ++iter) {
    const double value = evaluateQuarticDevice(a4, a3, a2, a1, a0, x);
    const double slope = evaluateQuarticDerivativeDevice(a4, a3, a2, a1, x);
    if (std::fabs(slope) <= tolerance) {
      break;
    }

    const double next = x - (value / slope);
    if (!std::isfinite(next)) {
      break;
    }
    if (std::fabs(next - x) <= tolerance) {
      x = next;
      break;
    }
    x = next;
  }
  return x;
}

TORIRENDER_ACC_ROUTINE_SEQ
void insertUniqueSortedQuarticRoot(QuarticRealRoots& roots,
                                   double root,
                                   double tolerance) noexcept {
  for (int i = 0; i < roots.count; ++i) {
    if (std::fabs(roots.values[static_cast<std::size_t>(i)] - root) <= (8.0 * tolerance)) {
      return;
    }
  }

  int position = roots.count;
  for (int i = 0; i < roots.count; ++i) {
    if (root < roots.values[static_cast<std::size_t>(i)]) {
      position = i;
      break;
    }
  }

  if (roots.count < static_cast<int>(roots.values.size())) {
    for (int i = roots.count; i > position; --i) {
      roots.values[static_cast<std::size_t>(i)] = roots.values[static_cast<std::size_t>(i - 1)];
    }
    roots.values[static_cast<std::size_t>(position)] = root;
    ++roots.count;
    return;
  }

  if (position >= static_cast<int>(roots.values.size())) {
    return;
  }

  for (int i = static_cast<int>(roots.values.size()) - 1; i > position; --i) {
    roots.values[static_cast<std::size_t>(i)] = roots.values[static_cast<std::size_t>(i - 1)];
  }
  roots.values[static_cast<std::size_t>(position)] = root;
}

TORIRENDER_ACC_ROUTINE_SEQ
QuarticRealRoots solveQuarticRealFixedOpenAcc(
    double a4, double a3, double a2, double a1, double a0, double tolerance) noexcept {
  QuarticRealRoots roots{};
  const double tol = sanitizeTolerance(tolerance);
  if (std::fabs(a4) <= tol) {
    return roots;
  }

  const double invA4 = 1.0 / a4;
  const double b3 = a3 * invA4;
  const double b2 = a2 * invA4;
  const double b1 = a1 * invA4;
  const double b0 = a0 * invA4;

  double maxCoeff = std::fabs(b3);
  maxCoeff = std::max(maxCoeff, std::fabs(b2));
  maxCoeff = std::max(maxCoeff, std::fabs(b1));
  maxCoeff = std::max(maxCoeff, std::fabs(b0));
  double bound = 1.0 + maxCoeff;
  if (!std::isfinite(bound) || bound <= 0.0) {
    bound = 1.0;
  }

  constexpr int kSampleCount = 256;
  const double step = (2.0 * bound) / static_cast<double>(kSampleCount);
  const double sampleTolerance = std::max(64.0 * tol, 1e-7);

  double previousX = -bound;
  double previousF = evaluateQuarticDevice(a4, a3, a2, a1, a0, previousX);

  for (int i = 1; i <= kSampleCount; ++i) {
    const double x = -bound + (static_cast<double>(i) * step);
    const double f = evaluateQuarticDevice(a4, a3, a2, a1, a0, x);

    if (std::fabs(f) <= sampleTolerance) {
      const double refined = refineQuarticNewtonDevice(a4, a3, a2, a1, a0, x, tol);
      if (std::isfinite(refined) &&
          std::fabs(evaluateQuarticDevice(a4, a3, a2, a1, a0, refined)) <= 1e-6) {
        insertUniqueSortedQuarticRoot(roots, refined, tol);
      }
    }

    if ((previousF > 0.0 && f < 0.0) || (previousF < 0.0 && f > 0.0)) {
      const double bracket = bisectQuarticDevice(a4, a3, a2, a1, a0, previousX, x);
      const double refined = refineQuarticNewtonDevice(a4, a3, a2, a1, a0, bracket, tol);
      if (std::isfinite(refined) &&
          std::fabs(evaluateQuarticDevice(a4, a3, a2, a1, a0, refined)) <= 1e-6) {
        insertUniqueSortedQuarticRoot(roots, refined, tol);
      }
    }

    previousX = x;
    previousF = f;
  }

  return roots;
}

std::vector<double> solvePolynomialReal(const std::array<double, 5>& input, double tolerance) {
  return toVector(solvePolynomialRealBuffer(input, tolerance));
}

}  // namespace

TORIRENDER_ACC_ROUTINE_SEQ
QuarticRealRoots solveQuarticRealFixed(
    double a4, double a3, double a2, double a1, double a0, double tolerance) {
#if defined(TORIRENDER_USE_OPENACC)
  return solveQuarticRealFixedOpenAcc(a4, a3, a2, a1, a0, tolerance);
#else
  const RealRootBuffer roots = solvePolynomialRealBuffer({a4, a3, a2, a1, a0}, tolerance);
  QuarticRealRoots out{};
  out.count = static_cast<int>(std::min<std::size_t>(roots.count, out.values.size()));
  for (int i = 0; i < out.count; ++i) {
    out.values[static_cast<std::size_t>(i)] = roots.values[static_cast<std::size_t>(i)];
  }
  return out;
#endif
}

std::vector<double> solveQuarticReal(
    double a4, double a3, double a2, double a1, double a0, double tolerance) {
  return solvePolynomialReal({a4, a3, a2, a1, a0}, tolerance);
}

}  // namespace torirender::math
