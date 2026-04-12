#pragma once

#include <limits>

#include "core/Accel.hpp"
#include "core/Hit.hpp"
#include "core/Ray.hpp"
#include "geometry/BVH.hpp"
#include "geometry/Torus.hpp"
#include "render/Shader.hpp"
#include "scene/SceneConfig.hpp"

namespace torirender {

class Scene {
 public:
  enum class MaterialType { Diffuse, Metal };

  struct Material {
    MaterialType type = MaterialType::Diffuse;
    Vec3 baseColor = Vec3(1.0, 1.0, 1.0);
    PhongParams phong{};
    double reflection = 0.0;
    double fuzz = 0.0;
  };

  Scene() noexcept;
  explicit Scene(const SceneConfig& config) noexcept;

  TORIRENDER_ACC_ROUTINE_SEQ
  bool hit(const Ray& ray,
           HitRecord& hitRecord,
           double tMin = 1e-4,
           double tMax = std::numeric_limits<double>::infinity()) const noexcept;

  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3 trace(const Ray& ray, int maxDepth = 2) const noexcept;

 private:
  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3 traceRecursive(const Ray& ray, int depth) const noexcept;
  TORIRENDER_ACC_ROUTINE_SEQ
  Vec3 background(const Ray& ray) const noexcept;
  TORIRENDER_ACC_ROUTINE_SEQ
  double directLightVisibility(const HitRecord& hitRecord) const noexcept;
  TORIRENDER_ACC_ROUTINE_SEQ
  bool hitFloor(const Ray& ray, HitRecord& hitRecord, double tMin, double tMax) const noexcept;
  TORIRENDER_ACC_ROUTINE_SEQ
  const Material& materialForTorusHit(const HitRecord& hitRecord) const noexcept;
  TORIRENDER_ACC_ROUTINE_SEQ
  const Material& materialForFloorHit(const HitRecord& hitRecord) const noexcept;

  Torus torusA_;
  Torus torusB_;
  BVHNode bvh_;
  Shader shader_;
  Vec3 lightDirection_;
  Vec3 backgroundLow_;
  Vec3 backgroundHigh_;
  double floorY_;
  double floorCheckerScale_;
  Material torusMaterialA_;
  Material torusMaterialB_;
  Material floorLightMaterial_;
  Material floorDarkMaterial_;
};

}  // namespace torirender
