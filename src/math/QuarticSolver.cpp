#include "math/QuarticSolver.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace torirender::math {

namespace {

constexpr int kMaxIterations = 256;

std::complex<double> evaluatePolynomial(const std::vector<double>& coeffs,
                                        const std::complex<double>& x) {
  std::complex<double> result = coeffs[0];
  for (std::size_t i = 1; i < coeffs.size(); ++i) {
    result = (result * x) + coeffs[i];
  }
  return result;
}

double evaluatePolynomial(const std::vector<double>& coeffs, double x) {
  double result = coeffs[0];
  for (std::size_t i = 1; i < coeffs.size(); ++i) {
    result = (result * x) + coeffs[i];
  }
  return result;
}

double evaluatePolynomialDerivative(const std::vector<double>& coeffs, double x) {
  const int degree = static_cast<int>(coeffs.size()) - 1;
  if (degree <= 0) {
    return 0.0;
  }

  double result = coeffs[0] * degree;
  for (int i = 1; i < degree; ++i) {
    result = (result * x) + (coeffs[i] * (degree - i));
  }
  return result;
}

std::vector<double> trimLeadingZeros(const std::vector<double>& coeffs, double tolerance) {
  std::size_t first = 0;
  while (first + 1 < coeffs.size() && std::fabs(coeffs[first]) <= tolerance) {
    ++first;
  }
  return std::vector<double>(coeffs.begin() + static_cast<std::ptrdiff_t>(first), coeffs.end());
}

std::vector<double> solveLinear(double a, double b, double tolerance) {
  if (std::fabs(a) <= tolerance) {
    return {};
  }
  return {-b / a};
}

std::vector<double> solveQuadratic(double a, double b, double c, double tolerance) {
  if (std::fabs(a) <= tolerance) {
    return solveLinear(b, c, tolerance);
  }

  const double discriminant = (b * b) - (4.0 * a * c);
  if (discriminant < -tolerance) {
    return {};
  }
  if (std::fabs(discriminant) <= tolerance) {
    return {-b / (2.0 * a)};
  }

  const double sqrtDiscriminant = std::sqrt(discriminant);
  const double q = -0.5 * (b + std::copysign(sqrtDiscriminant, b));

  std::vector<double> roots;
  roots.reserve(2);
  roots.push_back(q / a);
  if (std::fabs(q) > tolerance) {
    roots.push_back(c / q);
  }
  return roots;
}

std::vector<std::complex<double>> durandKerner(const std::vector<double>& monicCoeffs,
                                               double tolerance) {
  const int degree = static_cast<int>(monicCoeffs.size()) - 1;
  std::vector<std::complex<double>> roots(static_cast<std::size_t>(degree));
  std::vector<std::complex<double>> nextRoots(static_cast<std::size_t>(degree));

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

      const std::complex<double> delta = evaluatePolynomial(monicCoeffs, zi) / denom;
      nextRoots[static_cast<std::size_t>(i)] = zi - delta;
      maxDelta = std::max(maxDelta, std::abs(delta));
    }

    roots.swap(nextRoots);
    if (maxDelta <= tolerance) {
      break;
    }
  }

  return roots;
}

double refineRealRootNewton(const std::vector<double>& coeffs, double initial, double tolerance) {
  double x = initial;
  for (int iter = 0; iter < 16; ++iter) {
    const double value = evaluatePolynomial(coeffs, x);
    const double slope = evaluatePolynomialDerivative(coeffs, x);
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

double rootBound(const std::vector<double>& monicCoeffs) {
  double maxCoeff = 0.0;
  for (std::size_t i = 1; i < monicCoeffs.size(); ++i) {
    maxCoeff = std::max(maxCoeff, std::fabs(monicCoeffs[i]));
  }
  return 1.0 + maxCoeff;
}

double bisectRoot(const std::vector<double>& coeffs, double left, double right) {
  double fLeft = evaluatePolynomial(coeffs, left);

  for (int iter = 0; iter < 80; ++iter) {
    const double mid = 0.5 * (left + right);
    const double fMid = evaluatePolynomial(coeffs, mid);
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

void collectRootsFromSampling(const std::vector<double>& coeffs,
                              const std::vector<double>& monicCoeffs,
                              double tolerance,
                              std::vector<double>& roots) {
  const double bound = rootBound(monicCoeffs);
  constexpr int kSamples = 4096;
  const double step = (2.0 * bound) / static_cast<double>(kSamples);
  const double sampleTolerance = 100.0 * tolerance;

  double previousX = -bound;
  double previousF = evaluatePolynomial(coeffs, previousX);

  for (int i = 1; i <= kSamples; ++i) {
    const double x = -bound + (static_cast<double>(i) * step);
    const double f = evaluatePolynomial(coeffs, x);

    if (std::fabs(f) <= sampleTolerance) {
      const double refined = refineRealRootNewton(coeffs, x, tolerance);
      if (std::isfinite(refined) && std::fabs(evaluatePolynomial(coeffs, refined)) <= 1e-6) {
        roots.push_back(refined);
      }
    }

    if ((previousF > 0.0 && f < 0.0) || (previousF < 0.0 && f > 0.0)) {
      const double bracketRoot = bisectRoot(coeffs, previousX, x);
      const double refined = refineRealRootNewton(coeffs, bracketRoot, tolerance);
      if (std::isfinite(refined) && std::fabs(evaluatePolynomial(coeffs, refined)) <= 1e-6) {
        roots.push_back(refined);
      }
    }

    previousX = x;
    previousF = f;
  }
}

void sortAndUniqueRoots(std::vector<double>& roots, double tolerance) {
  std::sort(roots.begin(), roots.end());

  std::vector<double> uniqueRoots;
  uniqueRoots.reserve(roots.size());

  for (double root : roots) {
    if (uniqueRoots.empty() || std::fabs(root - uniqueRoots.back()) > (8.0 * tolerance)) {
      uniqueRoots.push_back(root);
    }
  }

  roots.swap(uniqueRoots);
}

std::vector<double> solvePolynomialReal(std::vector<double> coeffs, double tolerance) {
  coeffs = trimLeadingZeros(coeffs, tolerance);
  const int degree = static_cast<int>(coeffs.size()) - 1;
  if (degree <= 0) {
    return {};
  }
  if (degree == 1) {
    return solveLinear(coeffs[0], coeffs[1], tolerance);
  }
  if (degree == 2) {
    return solveQuadratic(coeffs[0], coeffs[1], coeffs[2], tolerance);
  }

  const double leading = coeffs[0];
  if (std::fabs(leading) <= tolerance) {
    return {};
  }

  std::vector<double> monic = coeffs;
  for (double& c : monic) {
    c /= leading;
  }

  const std::vector<std::complex<double>> complexRoots = durandKerner(monic, tolerance);

  std::vector<double> realRoots;
  realRoots.reserve(complexRoots.size() + 8);

  const double imagTolerance = std::sqrt(tolerance);
  for (const std::complex<double>& root : complexRoots) {
    if (std::fabs(root.imag()) > imagTolerance || !std::isfinite(root.real())) {
      continue;
    }

    const double refined = refineRealRootNewton(coeffs, root.real(), tolerance);

    const double residual = std::fabs(evaluatePolynomial(coeffs, refined));
    if (residual <= 1e-7) {
      realRoots.push_back(refined);
    }
  }

  collectRootsFromSampling(coeffs, monic, tolerance, realRoots);

  sortAndUniqueRoots(realRoots, tolerance);
  return realRoots;
}

}  // namespace

std::vector<double> solveQuarticReal(
    double a4, double a3, double a2, double a1, double a0, double tolerance) {
  return solvePolynomialReal({a4, a3, a2, a1, a0}, tolerance);
}

}  // namespace torirender::math
