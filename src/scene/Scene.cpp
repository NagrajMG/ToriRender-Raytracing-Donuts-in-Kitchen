#include "scene/Scene.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace torirender {

namespace {

// convert string to lowercase
std::string toLower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

// parse axis string to enum
TorusAxis parseAxis(const std::string& axisToken) noexcept {
  const std::string axis = toLower(axisToken);
  if (axis == "x")
    return TorusAxis::X;
  if (axis == "z")
    return TorusAxis::Z;
  return TorusAxis::Y;
}

// build torus from config (custom axis supported)
Torus buildTorus(const TorusConfig& config) noexcept {
  if (config.hasAxisDirection) {
    return Torus(config.majorRadius, config.minorRadius, config.center, config.axisDirection);
  }
  return Torus(config.majorRadius, config.minorRadius, config.center, parseAxis(config.axis));
}

// parse material type
Scene::MaterialType parseMaterialType(const std::string& token) noexcept {
  const std::string type = toLower(token);
  if (type == "metal")
    return Scene::MaterialType::Metal;
  return Scene::MaterialType::Diffuse;
}

// reflect vector
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 reflect(const Vec3& direction, const Vec3& normal) noexcept {
  return direction - (2.0 * dot(direction, normal) * normal);
}

// simple hash to [0,1]
TORIRENDER_ACC_ROUTINE_SEQ
double hash01(const Vec3& seed, double offset) noexcept {
  const Vec3 q = seed + Vec3(offset, offset * 1.7, offset * 2.3);
  const double s = std::sin(dot(q, Vec3(12.9898, 78.233, 37.719))) * 43758.5453123;
  return s - std::floor(s);
}

// random unit vector (used for fuzz)
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 randomUnitVector(const Vec3& seed, int depth) noexcept {
  constexpr double kTwoPi = 6.283185307179586476925286766559;
  const double u = hash01(seed, 7.0 + static_cast<double>(depth) * 0.31);
  const double v = hash01(seed, 31.0 + static_cast<double>(depth) * 0.17);
  const double azimuth = kTwoPi * u;
  const double z = (2.0 * v) - 1.0;
  const double r = std::sqrt(std::max(0.0, 1.0 - (z * z)));
  return Vec3(r * std::cos(azimuth), z, r * std::sin(azimuth));
}

// component-wise multiply
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 multiply(const Vec3& a, const Vec3& b) noexcept {
  return Vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}

// convert config to runtime
Scene::Material makeMaterial(const MaterialConfig& config) noexcept {
  Scene::Material material{};
  material.type = parseMaterialType(config.type);
  material.baseColor = config.baseColor;
  material.phong.ambientStrength = config.ambient;
  material.phong.diffuseStrength = config.diffuse;
  material.phong.specularStrength = config.specular;
  material.phong.shininess = config.shininess;
  material.phong.specularColor = config.specularColor;
  material.reflection = config.reflection;
  material.fuzz = std::max(0.0, std::min(1.0, config.fuzz));
  return material;
}

}  // namespace

// default scene
Scene::Scene() noexcept : Scene(defaultSceneConfig()) {
}

// build full scene
Scene::Scene(const SceneConfig& config) noexcept
    : torusA_(buildTorus(config.torusPrimary)),
      torusB_(buildTorus(config.torusSecondary)),
      bvh_(torusA_, torusB_),
      shader_(),
      lightDirection_(config.lightDirection.normalized()),
      backgroundLow_(config.backgroundLow),
      backgroundHigh_(config.backgroundHigh),
      floorY_(config.floorY),
      floorCheckerScale_(config.floorCheckerScale),
      torusMaterialA_(makeMaterial(config.torusPrimary.material)),
      torusMaterialB_(makeMaterial(config.torusSecondary.material)),
      floorLightMaterial_(makeMaterial(config.floorLightMaterial)),
      floorDarkMaterial_(makeMaterial(config.floorDarkMaterial)) {
}

// BVH hit test
TORIRENDER_ACC_ROUTINE_SEQ
bool Scene::hit(const Ray& ray, HitRecord& hitRecord, double tMin, double tMax) const noexcept {
  return bvh_.hit(ray, hitRecord, tMin, tMax);
}

// entry ray tracing
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 Scene::trace(const Ray& ray, int maxDepth) const noexcept {
  const int depth = maxDepth;
  return traceRecursive(ray, depth);
}

// recursive ray tracing
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 Scene::traceRecursive(const Ray& ray, int depth) const noexcept {
  Ray currentRay = ray;
  Vec3 throughput(1.0, 1.0, 1.0);
  Vec3 radiance{};
  int depthRemaining = depth;

  while (true) {
    HitRecord torusHit{};
    const bool hitTorus = hit(currentRay, torusHit, 1e-4);

    HitRecord floorHit{};
    const bool hitFloorSurface =
        hitFloor(currentRay,
                 floorHit,
                 1e-4,
                 hitTorus ? torusHit.t : std::numeric_limits<double>::infinity());

    if (!hitTorus && !hitFloorSurface) {
      radiance += multiply(throughput, background(currentRay));
      return radiance;
    }

    const bool useFloor = hitFloorSurface && (!hitTorus || floorHit.t < torusHit.t);
    const HitRecord& hitRecord = useFloor ? floorHit : torusHit;
    const Material& material =
        useFloor ? materialForFloorHit(hitRecord) : materialForTorusHit(hitRecord);

    const Vec3 rayDirection = currentRay.direction();
    const Vec3 viewDirection = -rayDirection;
    // Optimization worked out here: skip shadow-ray traversal when light is behind the surface.
    const double visibility =
        dot(hitRecord.normal, lightDirection_) > 0.0 ? directLightVisibility(hitRecord) : 0.0;

    const Vec3 localShading = shader_.shade(material.baseColor,
                                            hitRecord.normal,
                                            lightDirection_,
                                            viewDirection,
                                            material.phong,
                                            visibility,
                                            material.type == MaterialType::Metal);

    if (depthRemaining <= 0) {
      radiance += multiply(throughput, localShading);
      return radiance;
    }

    if (material.type == MaterialType::Metal) {
      Vec3 reflectedDirection = reflect(rayDirection, hitRecord.normal).normalized();
      reflectedDirection =
          (reflectedDirection + (material.fuzz * randomUnitVector(hitRecord.point, depthRemaining)))
              .normalized();

      if (dot(reflectedDirection, hitRecord.normal) <= 0.0) {
        radiance += multiply(throughput, 0.25 * localShading);
        return radiance;
      }

      radiance += multiply(throughput, 0.12 * localShading);
      throughput = multiply(throughput, 0.88 * material.baseColor);
      currentRay = Ray(hitRecord.point + (1e-4 * hitRecord.normal), reflectedDirection);
      --depthRemaining;
      continue;
    }

    const double reflection = std::max(0.0, std::min(1.0, material.reflection));
    if (reflection <= 0.0) {
      radiance += multiply(throughput, localShading);
      return radiance;
    }

    radiance += multiply(throughput, (1.0 - reflection) * localShading);
    throughput *= reflection;

    const Vec3 reflectedDirection = reflect(rayDirection, hitRecord.normal).normalized();
    currentRay = Ray(hitRecord.point + (1e-4 * hitRecord.normal), reflectedDirection);
    --depthRemaining;
  }
}

// sky gradient
TORIRENDER_ACC_ROUTINE_SEQ
Vec3 Scene::background(const Ray& ray) const noexcept {
  // Optimization worked out here: camera/reflection rays are already normalized.
  const double blend = 0.5 * (ray.direction().y + 1.0);
  return ((1.0 - blend) * backgroundLow_) + (blend * backgroundHigh_);
}

// shadow ray
TORIRENDER_ACC_ROUTINE_SEQ
double Scene::directLightVisibility(const HitRecord& hitRecord) const noexcept {
  // Optimization worked out here: lightDirection_ is pre-normalized in Scene constructor.
  const Vec3 shadowDirection = lightDirection_;
  if (shadowDirection.nearZero()) {
    return 1.0;
  }

  const double offsetSign = dot(shadowDirection, hitRecord.normal) >= 0.0 ? 1.0 : -1.0;

  const Ray shadowRay(hitRecord.point + ((1e-4 * offsetSign) * hitRecord.normal), shadowDirection);

  // blocked by torus
  HitRecord torusBlocker{};
  if (hit(shadowRay, torusBlocker, 1e-4, std::numeric_limits<double>::infinity())) {
    return 0.0;
  }

  // blocked by floor
  HitRecord floorBlocker{};
  if (hitFloor(shadowRay, floorBlocker, 1e-4, std::numeric_limits<double>::infinity())) {
    return 0.0;
  }

  return 1.0;
}

// ray-plane intersection (floor)
TORIRENDER_ACC_ROUTINE_SEQ
bool Scene::hitFloor(const Ray& ray,
                     HitRecord& hitRecord,
                     double tMin,
                     double tMax) const noexcept {
  const double dy = ray.direction().y;
  if (std::fabs(dy) <= 1e-12) {
    return false;
  }

  const double t = (floorY_ - ray.origin().y) / dy;
  if (t <= tMin || t >= tMax) {
    return false;
  }

  hitRecord.t = t;
  hitRecord.point = ray.at(t);
  hitRecord.primitiveId = -1;
  hitRecord.setFaceNormal(ray, Vec3(0.0, 1.0, 0.0));

  return true;
}

// choose closest torus material
TORIRENDER_ACC_ROUTINE_SEQ
const Scene::Material& Scene::materialForTorusHit(const HitRecord& hitRecord) const noexcept {
  if (hitRecord.primitiveId == 0) {
    return torusMaterialA_;
  }
  if (hitRecord.primitiveId == 1) {
    return torusMaterialB_;
  }

  const double dA = std::fabs(torusA_.evaluate(hitRecord.point));
  const double dB = std::fabs(torusB_.evaluate(hitRecord.point));
  return dA <= dB ? torusMaterialA_ : torusMaterialB_;
}

// checkerboard floor
TORIRENDER_ACC_ROUTINE_SEQ
const Scene::Material& Scene::materialForFloorHit(const HitRecord& hitRecord) const noexcept {
  const int tx = static_cast<int>(std::floor(hitRecord.point.x * floorCheckerScale_));
  const int tz = static_cast<int>(std::floor(hitRecord.point.z * floorCheckerScale_));
  const bool lightTile = ((tx + tz) & 1) == 0;
  return lightTile ? floorLightMaterial_ : floorDarkMaterial_;
}

}  // namespace torirender
