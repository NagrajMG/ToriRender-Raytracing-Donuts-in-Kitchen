#include "render/Shader.hpp"

#include <algorithm>
#include <cmath>

#include "math/Utils.hpp"

namespace torirender {

namespace {

// clamp color to [0,1]
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 clamp01(const Vec3& value) noexcept {
  return Vec3(math::clamp(value.x, 0.0, 1.0),
              math::clamp(value.y, 0.0, 1.0),
              math::clamp(value.z, 0.0, 1.0));
}

// component-wise multiplication (color * color)
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 multiply(const Vec3& a, const Vec3& b) noexcept {
  return Vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}

}  // namespace

TORIRENDER_ACC_ROUTINE_SEQ
Vec3 Shader::shade(const Vec3& baseColor,
                   const Vec3& normal,
                   const Vec3& lightDirection,
                   const Vec3& viewDirection,
                   const PhongParams& params,
                   double directLightVisibility,
                   bool isMetal) const noexcept {
  // Optimization worked out here: inputs are already normalized from Scene.
  const Vec3& n = normal;
  const Vec3& l = lightDirection;
  const Vec3& v = viewDirection;

  const double visibility = math::clamp(directLightVisibility, 0.0, 1.0);

  // ambient term
  const Vec3 ambient = params.ambientStrength * baseColor;

  // Lambert diffuse term
  const double nDotL = std::max(dot(n, l), 0.0);
  if (nDotL <= 0.0) {
    return ambient;  // light behind surface
  }

  // half vector (Blinn-Phong)
  Vec3 halfVector = (l + v);
  if (halfVector.nearZero()) {
    halfVector = l;
  }
  halfVector.normalize();

  // Fresnel (Schlick approximation)
  const Vec3 f0 = isMetal ? clamp01(baseColor) : Vec3(0.04, 0.04, 0.04);
  const double vDotH = std::max(dot(v, halfVector), 0.0);
  const double fresnelFactor = std::pow(1.0 - vDotH, 5.0);
  const Vec3 fresnel = f0 + ((Vec3(1.0, 1.0, 1.0) - f0) * fresnelFactor);

  // specular term
  const double nDotH = std::max(dot(n, halfVector), 0.0);
  const double specTerm = std::pow(nDotH, params.shininess);
  const Vec3 specular = (visibility * params.specularStrength * nDotL * specTerm) *
                        multiply(fresnel, params.specularColor);

  Vec3 diffuse{};

  // diffuse only for non-metal
  if (!isMetal) {
    const Vec3 kd = Vec3(1.0, 1.0, 1.0) - fresnel;  // energy conservation
    diffuse = (visibility * params.diffuseStrength * nDotL) * multiply(kd, baseColor);
  }

  // final color
  return ambient + diffuse + specular;
}

}  // namespace torirender
