#pragma once

#include <cstdint>
#include <string>

#include "math/Vec3.hpp"

namespace torirender {

// defaulter fallbacks
struct MaterialConfig {
  std::string type = "diffuse";
  Vec3 baseColor = Vec3(1.0, 1.0, 1.0);
  Vec3 specularColor = Vec3(1.0, 1.0, 1.0);
  double ambient = 0.18;
  double diffuse = 0.68;
  double specular = 0.36;
  double shininess = 48.0;
  double reflection = 0.0;
  double fuzz = 0.0;
};

struct TorusConfig {
  double majorRadius = 1.0;
  double minorRadius = 0.3;
  Vec3 center = Vec3(0.0, 0.0, -3.0);
  std::string axis = "y";
  bool hasAxisDirection = false;
  Vec3 axisDirection = Vec3(0.0, 1.0, 0.0);
  MaterialConfig material{};
};

struct CameraConfig {
  Vec3 lookFrom = Vec3(0.0, 1.2, 2.5);
  Vec3 lookAt = Vec3(0.0, 0.0, -3.0);
  Vec3 viewUp = Vec3(0.0, 1.0, 0.0);
  double vfov = 40.0;
  int imageWidth = 640;
  int imageHeight = 360;
  int maxDepth = 2;
  int samplesPerPixel = 1;
  std::uint64_t rngSeed = 1337ULL;
};

struct RuntimeConfig {
  std::string mode = "serial";
  int mpiRanks = 1;
  int ompThreads = 1;
  int heartbeatSeconds = 60;
};

struct SceneConfig {
  CameraConfig camera{};
  RuntimeConfig runtime{};
  TorusConfig torusPrimary{};
  TorusConfig torusSecondary{};
  Vec3 lightDirection = Vec3(-0.34, 1.0, -0.25);
  Vec3 backgroundLow = Vec3(0.18, 0.22, 0.32);
  Vec3 backgroundHigh = Vec3(0.78, 0.88, 1.0);
  double floorY = -1.22;
  double floorCheckerScale = 1.35;
  MaterialConfig floorLightMaterial{};
  MaterialConfig floorDarkMaterial{};
};

SceneConfig defaultSceneConfig() noexcept;

// configs loader
bool loadSceneConfigFromJsonFile(const std::string& path,
                                 SceneConfig& outConfig,
                                 std::string* errorMessage = nullptr);

}  // namespace torirender
