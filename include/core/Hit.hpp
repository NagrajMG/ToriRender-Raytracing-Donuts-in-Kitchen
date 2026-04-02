#pragma once

#include "core/Ray.hpp"

namespace torirender {

struct HitRecord {
  double t = 0.0;

  Vec3 point{};

  Vec3 normal{};

  int primitiveId = -1;

  bool frontFace = true;

  void setFaceNormal(const Ray& ray, const Vec3& outwardNormal) noexcept {
    frontFace = dot(ray.direction(), outwardNormal) < 0.0;
    normal = frontFace ? outwardNormal : -outwardNormal;
  }
};

}  // namespace torirender
