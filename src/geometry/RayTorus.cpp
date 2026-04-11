// basically returns the intersection point of torus and ray by refining
#include "geometry/RayTorus.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "geometry/RayMarcher.hpp"
#include "math/QuarticSolver.hpp"

namespace torirender {

namespace {

struct QuarticCoefficients {
  double a4;
  double a3;
  double a2;
  double a1;
  double a0;
};

QuarticCoefficients buildQuartic(const Ray& ray, const Torus& torus) noexcept {
  const Vec3 axis = torus.axisDirection();
  const Vec3 o = ray.origin() - torus.center();
  const Vec3 d = ray.direction();

  const double majorRadius = torus.majorRadius();
  const double minorRadius = torus.minorRadius();
  const double majorRadius2 = majorRadius * majorRadius;
  const double minorRadius2 = minorRadius * minorRadius;

  const double g2 = dot(d, d);
  const double g1 = 2.0 * dot(o, d);
  const double g0 = dot(o, o);

  const double s1 = dot(axis, d);
  const double s0 = dot(axis, o);

  const double k = majorRadius2 - minorRadius2;
  const double m = -2.0 * (majorRadius2 + minorRadius2);

  QuarticCoefficients coeffs{};
  coeffs.a4 = g2 * g2;
  coeffs.a3 = 2.0 * g2 * g1;
  coeffs.a2 = (g1 * g1) + (2.0 * g2 * g0) + (m * g2) + (4.0 * majorRadius2 * s1 * s1);
  coeffs.a1 = (2.0 * g1 * g0) + (m * g1) + (8.0 * majorRadius2 * s1 * s0);
  coeffs.a0 = (g0 * g0) + (m * g0) + (k * k) + (4.0 * majorRadius2 * s0 * s0);
  return coeffs;
}

double evaluateQuartic(const QuarticCoefficients& coeffs, double t) noexcept {
  return (((coeffs.a4 * t + coeffs.a3) * t + coeffs.a2) * t + coeffs.a1) * t + coeffs.a0;
}

double evaluateQuarticDerivative(const QuarticCoefficients& coeffs, double t) noexcept {
  return ((4.0 * coeffs.a4 * t + 3.0 * coeffs.a3) * t + 2.0 * coeffs.a2) * t + coeffs.a1;
}

double refineRootNewton(const QuarticCoefficients& coeffs, double initialRoot) noexcept {
  double t = initialRoot;
  for (int iter = 0; iter < 8; ++iter) {
    // Optimization worked out here: Newton refinement on quartic polynomial avoids torus gradient
    // eval.
    const double f = evaluateQuartic(coeffs, t);
    const double fp = evaluateQuarticDerivative(coeffs, t);

    if (std::fabs(fp) <= 1e-12) {
      break;
    }

    const double next = t - (f / fp);
    if (!std::isfinite(next)) {
      break;
    }
    if (std::fabs(next - t) <= 1e-10) {
      t = next;
      break;
    }
    t = next;
  }
  return t;
}

}  // namespace

bool intersectRayTorus(const Ray& ray,
                       const Torus& torus,
                       HitRecord& hitRecord,
                       double tMin,
                       double tMax,
                       bool useSdfFallback) noexcept {
  const QuarticCoefficients coeffs = buildQuartic(ray, torus);
  std::vector<double> roots =
      math::solveQuarticReal(coeffs.a4, coeffs.a3, coeffs.a2, coeffs.a1, coeffs.a0);

  bool found = false;
  double bestT = tMax;
  for (double root : roots) {
    if (!std::isfinite(root) || root <= tMin || root >= bestT) {
      continue;
    }

    const double refined = refineRootNewton(coeffs, root);
    if (!std::isfinite(refined) || refined <= tMin || refined >= bestT || refined >= tMax) {
      continue;
    }

    // Optimization worked out here: cheap quartic residual test inside root scan.
    const double residual = std::fabs(evaluateQuartic(coeffs, refined));
    if (residual > 1e-7) {
      continue;
    }

    bestT = refined;
    found = true;
  }

  if (!found) {
    if (!useSdfFallback) {
      return false;
    }
    return marchRayToTorus(ray, torus, hitRecord, tMin, tMax);
  }

  // Optimization worked out here: expensive torus residual validation is done once for final best
  // root.
  const Vec3 bestPoint = ray.at(bestT);
  if (std::fabs(torus.evaluate(bestPoint)) > 1e-5) {
    if (!useSdfFallback) {
      return false;
    }
    return marchRayToTorus(ray, torus, hitRecord, tMin, tMax);
  }

  hitRecord.t = bestT;
  hitRecord.point = bestPoint;
  const Vec3 outwardNormal = torus.normal(hitRecord.point, 1e-8);
  hitRecord.setFaceNormal(ray, outwardNormal);
  return true;
}

}  // namespace torirender
