#include "scene/SceneConfig.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#if defined(TORIRENDER_HAS_JSON)
#include <nlohmann/json.hpp>
#endif

namespace torirender {

SceneConfig defaultSceneConfig() noexcept {
  SceneConfig config{};

  config.camera.lookFrom = Vec3(0.0, 1.2, 2.5);
  config.camera.lookAt = Vec3(0.0, 0.0, -3.0);
  config.camera.viewUp = Vec3(0.0, 1.0, 0.0);
  config.camera.vfov = 40.0;
  config.camera.imageWidth = 640;
  config.camera.imageHeight = 360;
  config.camera.maxDepth = 2;
  config.camera.samplesPerPixel = 1;
  config.camera.rngSeed = 1337ULL;

  config.floorY = -1.22;
  config.floorCheckerScale = 1.35;

  config.torusPrimary.majorRadius = 1.12;
  config.torusPrimary.minorRadius = 0.34;
  config.torusPrimary.center = Vec3(-0.35, config.floorY + config.torusPrimary.minorRadius, -3.45);
  config.torusPrimary.axis = "y";
  config.torusPrimary.hasAxisDirection = false;
  config.torusPrimary.axisDirection = Vec3(0.0, 1.0, 0.0);
  config.torusPrimary.material.type = "metal";
  config.torusPrimary.material.baseColor = Vec3(1.0, 0.766, 0.336);
  config.torusPrimary.material.specularColor = Vec3(0.95, 0.95, 0.95);
  config.torusPrimary.material.ambient = 0.03;
  config.torusPrimary.material.diffuse = 0.10;
  config.torusPrimary.material.specular = 1.00;
  config.torusPrimary.material.shininess = 260.0;
  config.torusPrimary.material.fuzz = 0.08;

  config.torusSecondary.majorRadius = 0.95;
  config.torusSecondary.minorRadius = 0.31;
  config.torusSecondary.axis = "custom";
  config.torusSecondary.hasAxisDirection = true;
  config.torusSecondary.axisDirection = Vec3(0.58, 0.67, 0.46).normalized();
  const double secondaryAxisY = std::fabs(config.torusSecondary.axisDirection.y);
  const double secondaryVerticalExtent =
      (config.torusSecondary.majorRadius *
       std::sqrt(std::max(0.0, 1.0 - (secondaryAxisY * secondaryAxisY)))) +
      config.torusSecondary.minorRadius;
  config.torusSecondary.center = Vec3(1.11, config.floorY + secondaryVerticalExtent, -3.09);
  config.torusSecondary.material.type = "metal";
  config.torusSecondary.material.baseColor = Vec3(0.95, 0.95, 0.95);
  config.torusSecondary.material.specularColor = Vec3(1.0, 1.0, 1.0);
  config.torusSecondary.material.ambient = 0.03;
  config.torusSecondary.material.diffuse = 0.10;
  config.torusSecondary.material.specular = 1.00;
  config.torusSecondary.material.shininess = 260.0;
  config.torusSecondary.material.fuzz = 0.05;

  config.lightDirection = Vec3(-0.34, 1.0, -0.25);
  config.backgroundLow = Vec3(0.18, 0.22, 0.32);
  config.backgroundHigh = Vec3(0.78, 0.88, 1.0);

  config.floorLightMaterial.type = "diffuse";
  config.floorLightMaterial.baseColor = Vec3(0.88, 0.88, 0.90);
  config.floorLightMaterial.specularColor = Vec3(0.95, 0.95, 0.95);
  config.floorLightMaterial.ambient = 0.08;
  config.floorLightMaterial.diffuse = 0.72;
  config.floorLightMaterial.specular = 0.12;
  config.floorLightMaterial.shininess = 38.0;
  config.floorLightMaterial.reflection = 0.10;

  config.floorDarkMaterial.type = "diffuse";
  config.floorDarkMaterial.baseColor = Vec3(0.04, 0.04, 0.05);
  config.floorDarkMaterial.specularColor = Vec3(0.55, 0.55, 0.58);
  config.floorDarkMaterial.ambient = 0.03;
  config.floorDarkMaterial.diffuse = 0.58;
  config.floorDarkMaterial.specular = 0.08;
  config.floorDarkMaterial.shininess = 28.0;
  config.floorDarkMaterial.reflection = 0.05;

  return config;
}

#if defined(TORIRENDER_HAS_JSON)

namespace {

using Json = nlohmann::json;

bool parseVec3(const Json& value, Vec3& out) {
  if (value.is_array() && value.size() == 3) {
    out = Vec3(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
    return true;
  }
  if (value.is_object() && value.contains("x") && value.contains("y") && value.contains("z")) {
    out = Vec3(value["x"].get<double>(), value["y"].get<double>(), value["z"].get<double>());
    return true;
  }
  return false;
}

void updateMaterial(const Json& node, MaterialConfig& material) {
  if (!node.is_object()) {
    return;
  }

  if (node.contains("type")) {
    material.type = node["type"].get<std::string>();
  }
  if (node.contains("base_color")) {
    parseVec3(node["base_color"], material.baseColor);
  }
  if (node.contains("specular_color")) {
    parseVec3(node["specular_color"], material.specularColor);
  }
  if (node.contains("ambient")) {
    material.ambient = node["ambient"].get<double>();
  }
  if (node.contains("diffuse")) {
    material.diffuse = node["diffuse"].get<double>();
  }
  if (node.contains("specular")) {
    material.specular = node["specular"].get<double>();
  }
  if (node.contains("shininess")) {
    material.shininess = node["shininess"].get<double>();
  }
  if (node.contains("reflection")) {
    material.reflection = node["reflection"].get<double>();
  }
  if (node.contains("fuzz")) {
    material.fuzz = node["fuzz"].get<double>();
  }
}

void updateTorus(const Json& node, TorusConfig& torus) {
  if (!node.is_object()) {
    return;
  }

  if (node.contains("major_radius")) {
    torus.majorRadius = node["major_radius"].get<double>();
  }
  if (node.contains("minor_radius")) {
    torus.minorRadius = node["minor_radius"].get<double>();
  }
  if (node.contains("center")) {
    parseVec3(node["center"], torus.center);
  }
  if (node.contains("axis")) {
    torus.axis = node["axis"].get<std::string>();
  }
  if (node.contains("axis_direction")) {
    Vec3 parsed;
    if (parseVec3(node["axis_direction"], parsed)) {
      torus.axisDirection = parsed.normalized();
      torus.hasAxisDirection = true;
    }
  }
  if (node.contains("material")) {
    updateMaterial(node["material"], torus.material);
  }
}

}  // namespace

#endif

bool loadSceneConfigFromJsonFile(const std::string& path,
                                 SceneConfig& outConfig,
                                 std::string* errorMessage) {
  outConfig = defaultSceneConfig();

#if !defined(TORIRENDER_HAS_JSON)
  if (errorMessage != nullptr) {
    *errorMessage = "JSON support is unavailable (nlohmann_json not linked).";
  }
  (void)path;
  return false;
#else
  std::ifstream input(path);
  if (!input) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed to open config file: " + path;
    }
    return false;
  }

  Json root;
  try {
    input >> root;
  } catch (const std::exception& ex) {
    if (errorMessage != nullptr) {
      *errorMessage = std::string("Failed to parse JSON: ") + ex.what();
    }
    return false;
  }

  try {
    if (root.contains("camera") && root["camera"].is_object()) {
      const Json& camera = root["camera"];
      if (camera.contains("look_from")) {
        parseVec3(camera["look_from"], outConfig.camera.lookFrom);
      }
      if (camera.contains("look_at")) {
        parseVec3(camera["look_at"], outConfig.camera.lookAt);
      }
      if (camera.contains("view_up")) {
        parseVec3(camera["view_up"], outConfig.camera.viewUp);
      }
      if (camera.contains("vfov")) {
        outConfig.camera.vfov = camera["vfov"].get<double>();
      }
      if (camera.contains("image_width")) {
        outConfig.camera.imageWidth = camera["image_width"].get<int>();
      }
      if (camera.contains("image_height")) {
        outConfig.camera.imageHeight = camera["image_height"].get<int>();
      }
      if (camera.contains("max_depth")) {
        outConfig.camera.maxDepth = camera["max_depth"].get<int>();
      }
      if (camera.contains("samples_per_pixel")) {
        outConfig.camera.samplesPerPixel = camera["samples_per_pixel"].get<int>();
      }
      if (camera.contains("rng_seed")) {
        outConfig.camera.rngSeed = camera["rng_seed"].get<std::uint64_t>();
      }
    }

    if (root.contains("runtime") && root["runtime"].is_object()) {
      const Json& runtime = root["runtime"];
      if (runtime.contains("mode")) {
        outConfig.runtime.mode = runtime["mode"].get<std::string>();
      }
      if (runtime.contains("mpi_ranks")) {
        outConfig.runtime.mpiRanks = runtime["mpi_ranks"].get<int>();
      }
      if (runtime.contains("omp_threads")) {
        outConfig.runtime.ompThreads = runtime["omp_threads"].get<int>();
      }
      if (runtime.contains("heartbeat_seconds")) {
        outConfig.runtime.heartbeatSeconds = runtime["heartbeat_seconds"].get<int>();
      }
    }

    if (root.contains("torus_primary")) {
      updateTorus(root["torus_primary"], outConfig.torusPrimary);
    }
    if (root.contains("torus_secondary")) {
      updateTorus(root["torus_secondary"], outConfig.torusSecondary);
    }

    if (root.contains("scene") && root["scene"].is_object()) {
      const Json& scene = root["scene"];
      if (scene.contains("light_direction")) {
        parseVec3(scene["light_direction"], outConfig.lightDirection);
      }
      if (scene.contains("background_low")) {
        parseVec3(scene["background_low"], outConfig.backgroundLow);
      }
      if (scene.contains("background_high")) {
        parseVec3(scene["background_high"], outConfig.backgroundHigh);
      }
      if (scene.contains("floor_y")) {
        outConfig.floorY = scene["floor_y"].get<double>();
      }
      if (scene.contains("floor_checker_scale")) {
        outConfig.floorCheckerScale = scene["floor_checker_scale"].get<double>();
      }
      if (scene.contains("floor_light_material")) {
        updateMaterial(scene["floor_light_material"], outConfig.floorLightMaterial);
      }
      if (scene.contains("floor_dark_material")) {
        updateMaterial(scene["floor_dark_material"], outConfig.floorDarkMaterial);
      }
    }
  } catch (const std::exception& ex) {
    if (errorMessage != nullptr) {
      *errorMessage = std::string("Invalid config schema: ") + ex.what();
    }
    return false;
  }

  return true;
#endif
}

}  // namespace torirender
