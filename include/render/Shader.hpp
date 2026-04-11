#pragma once

#include "math/Vec3.hpp"

namespace torirender {

struct PhongParams {
  double ambientStrength = 0.18;
  double diffuseStrength = 0.68;
  double specularStrength = 0.36;
  double shininess = 48.0;
  Vec3 specularColor = Vec3(1.0, 1.0, 1.0);
};

class Shader {
 public:
  // Optimization worked out here: normal/lightDirection/viewDirection are expected normalized.
  Vec3 shade(const Vec3& baseColor,
             const Vec3& normal,
             const Vec3& lightDirection,
             const Vec3& viewDirection,
             const PhongParams& params,
             double directLightVisibility,
             bool isMetal) const noexcept;
};

}  // namespace torirender
