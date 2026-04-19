#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "core/Camera.hpp"
#include "io/Image.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneConfig.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

// Terminal args
struct RenderArgs {
  std::string configPath = "config/scene.json";
  std::string outputDir = "final";
};

struct ResolvedOutputPaths {
  std::filesystem::path outputDir;
  std::filesystem::path imagesDir;
  std::filesystem::path imagePath;
  std::string imageFileName;
  std::filesystem::path metricsCsvPath;
};

struct CliTheme {
  bool colorEnabled = false;
  const char* reset = "\033[0m";
  const char* title = "\033[1;33m";
  const char* section = "\033[1;34m";
  const char* key = "\033[1;37m";
  const char* value = "\033[1;32m";
  const char* accent = "\033[1;33m";
  const char* success = "\033[1;32m";
  const char* separator = "\033[1;36m";
  const char* muted = "\033[0;37m";
};

// connected to terminal???
bool supportsColor() noexcept {
#if defined(_WIN32)
  return false;
#else
  return std::getenv("NO_COLOR") == nullptr && ::isatty(fileno(stdout)) != 0;
#endif
}

std::string styled(const CliTheme& theme, const char* style, const std::string& text) {
  if (!theme.colorEnabled) {
    return text;
  }
  return std::string(style) + text + theme.reset;
}

std::string formatVec3(const torirender::Vec3& value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << '(' << value.x << ", " << value.y << ", " << value.z
      << ')';
  return out.str();
}

void printKeyValue(const CliTheme& theme,
                   const std::string& key,
                   const std::string& value,
                   int indent = 2) {
  std::cout << std::string(static_cast<std::size_t>(indent), ' ')
            << styled(theme, theme.key, key + ": ") << styled(theme, theme.value, value) << '\n';
}

void printMaterial(const CliTheme& theme,
                   const std::string& title,
                   const torirender::MaterialConfig& material) {
  std::cout << styled(theme, theme.section, title) << '\n';
  printKeyValue(theme, "Type", material.type);
  printKeyValue(theme, "Base Color", formatVec3(material.baseColor));
  printKeyValue(theme, "Specular Color", formatVec3(material.specularColor));
  printKeyValue(theme, "Ambient", std::to_string(material.ambient));
  printKeyValue(theme, "Diffuse", std::to_string(material.diffuse));
  printKeyValue(theme, "Specular", std::to_string(material.specular));
  printKeyValue(theme, "Shininess", std::to_string(material.shininess));
  printKeyValue(theme, "Reflection", std::to_string(material.reflection));
  printKeyValue(theme, "Fuzz", std::to_string(material.fuzz));
}

std::string axisDescription(const torirender::TorusConfig& torus) {
  if (torus.hasAxisDirection) {
    return std::string("custom ") + formatVec3(torus.axisDirection);
  }
  return torus.axis;
}

void printTorus(const CliTheme& theme,
                const std::string& title,
                const torirender::TorusConfig& torus) {
  std::cout << styled(theme, theme.section, title) << '\n';
  printKeyValue(theme, "Major Radius", std::to_string(torus.majorRadius));
  printKeyValue(theme, "Minor Radius", std::to_string(torus.minorRadius));
  printKeyValue(theme, "Center", formatVec3(torus.center));
  printKeyValue(theme, "Axis", axisDescription(torus));
  printMaterial(theme, "  Material", torus.material);
}

void printRenderConfiguration(const RenderArgs& args,
                              const ResolvedOutputPaths& paths,
                              const torirender::SceneConfig& config,
                              int width,
                              int height,
                              int samplesPerPixel) {
  CliTheme theme{};
  theme.colorEnabled = supportsColor();

  std::cout << '\n';
  std::cout << styled(theme, theme.title, "ToriRender - Render Configuration") << '\n';
  std::cout << styled(theme,
                      theme.separator,
                      "============================================================")
            << '\n';

  printKeyValue(theme, "Config File", args.configPath, 0);
  printKeyValue(theme, "Output Directory", paths.outputDir.string(), 0);
  printKeyValue(theme, "Image File", paths.imagePath.string(), 0);
  printKeyValue(theme, "Metrics CSV", paths.metricsCsvPath.string(), 0);
  printKeyValue(theme, "Resolution", std::to_string(width) + "x" + std::to_string(height), 0);
  printKeyValue(theme, "Samples/Pixel", std::to_string(samplesPerPixel), 0);
  printKeyValue(theme, "Max Depth", std::to_string(config.camera.maxDepth), 0);
  printKeyValue(theme, "RNG Seed", std::to_string(config.camera.rngSeed), 0);
  printKeyValue(
      theme,
      "Primary Samples",
      std::to_string(static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) *
                     static_cast<std::uint64_t>(samplesPerPixel)),
      0);

  std::cout << '\n' << styled(theme, theme.section, "Camera") << '\n';
  printKeyValue(theme, "Look From", formatVec3(config.camera.lookFrom));
  printKeyValue(theme, "Look At", formatVec3(config.camera.lookAt));
  printKeyValue(theme, "View Up", formatVec3(config.camera.viewUp));
  printKeyValue(theme, "Vertical FOV", std::to_string(config.camera.vfov));

  std::cout << '\n' << styled(theme, theme.section, "Scene Lighting") << '\n';
  printKeyValue(theme, "Light Direction", formatVec3(config.lightDirection));
  printKeyValue(theme, "Background Low", formatVec3(config.backgroundLow));
  printKeyValue(theme, "Background High", formatVec3(config.backgroundHigh));

  std::cout << '\n' << styled(theme, theme.section, "Floor") << '\n';
  printKeyValue(theme, "Floor Y", std::to_string(config.floorY));
  printKeyValue(theme, "Checker Scale", std::to_string(config.floorCheckerScale));
  printMaterial(theme, "  Light Tile Material", config.floorLightMaterial);
  printMaterial(theme, "  Dark Tile Material", config.floorDarkMaterial);

  std::cout << '\n';
  printTorus(theme, "Torus 1 (Primary)", config.torusPrimary);
  std::cout << '\n';
  printTorus(theme, "Torus 2 (Secondary)", config.torusSecondary);

  std::cout << '\n';
  std::cout << styled(theme, theme.accent, "Render started...") << '\n';
  std::cout << styled(theme,
                      theme.separator,
                      "============================================================")
            << "\n\n";
}

void printRenderCompleted(const std::string& imagePath,
                          const std::string& csvPath,
                          double elapsedSeconds,
                          int width,
                          int height,
                          int samplesPerPixel,
                          int maxDepth) {
  CliTheme theme{};
  theme.colorEnabled = supportsColor();

  std::cout << '\n';
  std::cout << styled(theme, theme.title, "Render completed successfully") << '\n';
  std::cout << styled(theme,
                      theme.separator,
                      "============================================================")
            << '\n';
  printKeyValue(theme, "Saved At", imagePath, 0);
  printKeyValue(theme, "Metrics CSV", csvPath, 0);
  std::ostringstream elapsedStream;
  elapsedStream << std::fixed << std::setprecision(3) << elapsedSeconds << " s";
  printKeyValue(theme, "Elapsed", elapsedStream.str(), 0);
  printKeyValue(theme,
                "Configuration",
                std::to_string(width) + "x" + std::to_string(height) + " | spp " +
                    std::to_string(samplesPerPixel) + " | depth " + std::to_string(maxDepth),
                0);
  std::cout << styled(theme,
                      theme.separator,
                      "============================================================")
            << '\n';
}

std::uint64_t splitMix64(std::uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

double randomUnit(std::uint64_t& state) noexcept {
  constexpr double kInv53 = 1.0 / 9007199254740992.0;  // 2^53
  const std::uint64_t bits = splitMix64(state);
  return static_cast<double>(bits >> 11) * kInv53;
}

std::string serialOutputFilename(int width, int height, int samplesPerPixel, int maxDepth) {
  return "serial_" + std::to_string(width) + "x" + std::to_string(height) + "_ssp" +
         std::to_string(samplesPerPixel) + "_depth" + std::to_string(maxDepth) + ".png";
}

bool resolveOutputPaths(const RenderArgs& args,
                        int width,
                        int height,
                        int samplesPerPixel,
                        int maxDepth,
                        ResolvedOutputPaths& outPaths,
                        std::string& errorMessage) {
  std::error_code ec;
  outPaths.outputDir = std::filesystem::path(args.outputDir);
  if (outPaths.outputDir.empty()) {
    outPaths.outputDir = "final";
  }

  std::filesystem::create_directories(outPaths.outputDir, ec);
  if (ec) {
    errorMessage = "Failed to create output directory: " + outPaths.outputDir.string() + " (" +
                   ec.message() + ")";
    return false;
  }

  outPaths.imagesDir = outPaths.outputDir / "images";
  std::filesystem::create_directories(outPaths.imagesDir, ec);
  if (ec) {
    errorMessage = "Failed to create images directory: " + outPaths.imagesDir.string() + " (" +
                   ec.message() + ")";
    return false;
  }

  outPaths.imageFileName = serialOutputFilename(width, height, samplesPerPixel, maxDepth);
  outPaths.imagePath = outPaths.imagesDir / outPaths.imageFileName;
  outPaths.metricsCsvPath = outPaths.outputDir / "serial_metrics.csv";
  return true;
}

bool appendRenderMetricsCsv(const std::filesystem::path& csvPath,
                            const std::string& imageFileName,
                            int width,
                            int height,
                            int samplesPerPixel,
                            int maxDepth,
                            double elapsedSeconds,
                            std::string& errorMessage) {
  constexpr const char* kLegacyHeader = "resolution,ssp,depth,time_seconds";
  constexpr const char* kCurrentHeader = "image_file,resolution,ssp,depth,time_seconds";

  auto migrateLegacySchema = [&](const std::filesystem::path& path) -> bool {
    std::ifstream input(path);
    if (!input) {
      errorMessage = "Failed to open metrics CSV for schema check: " + path.string();
      return false;
    }

    std::string header;
    if (!std::getline(input, header)) {
      return true;
    }

    if (header == kCurrentHeader) {
      return true;
    }
    if (header != kLegacyHeader) {
      errorMessage = "Unrecognized metrics CSV header in " + path.string();
      return false;
    }

    const std::filesystem::path tempPath = path.string() + ".tmp";
    std::ofstream output(tempPath, std::ios::trunc);
    if (!output) {
      errorMessage = "Failed to open temporary metrics CSV file: " + tempPath.string();
      return false;
    }

    output << kCurrentHeader << '\n';
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) {
        continue;
      }
      output << "legacy_unknown.png," << line << '\n';
    }

    if (!output) {
      errorMessage = "Failed while migrating legacy metrics CSV schema: " + path.string();
      return false;
    }

    std::error_code ec;
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
      std::filesystem::remove(tempPath, ec);
      errorMessage =
          "Failed to replace legacy metrics CSV: " + path.string() + " (" + ec.message() + ")";
      return false;
    }
    return true;
  };

  bool writeHeader = true;
  std::error_code ec;
  if (std::filesystem::exists(csvPath, ec)) {
    if (ec) {
      errorMessage =
          "Failed to access metrics CSV path: " + csvPath.string() + " (" + ec.message() + ")";
      return false;
    }
    const auto size = std::filesystem::file_size(csvPath, ec);
    if (ec) {
      errorMessage =
          "Failed to query metrics CSV size: " + csvPath.string() + " (" + ec.message() + ")";
      return false;
    }

    if (size > 0 && !migrateLegacySchema(csvPath)) {
      return false;
    }
    writeHeader = size == 0;
  }

  std::ofstream csv(csvPath, std::ios::app);
  if (!csv) {
    errorMessage = "Failed to open metrics CSV for append: " + csvPath.string();
    return false;
  }

  if (writeHeader) {
    csv << kCurrentHeader << '\n';
  }

  csv << imageFileName << ',' << width << 'x' << height << ',' << samplesPerPixel << ',' << maxDepth
      << ',' << std::fixed << std::setprecision(6) << elapsedSeconds << '\n';
  if (!csv) {
    errorMessage = "Failed to write metrics CSV row: " + csvPath.string();
    return false;
  }
  return true;
}

RenderArgs parseArgs(int argc, char** argv) {
  RenderArgs args{};
  if (argc >= 2) {
    args.configPath = std::string(argv[1]);
  }
  if (argc >= 3) {
    args.outputDir = std::string(argv[2]);
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  const RenderArgs args = parseArgs(argc, argv);

  torirender::SceneConfig config{};
  std::string errorMessage;
  if (!torirender::loadSceneConfigFromJsonFile(args.configPath, config, &errorMessage)) {
    std::cerr << "Failed to load scene config from " << args.configPath << ": " << errorMessage
              << '\n';
    return 1;
  }

  const int width = std::max(config.camera.imageWidth, 1);
  const int height = std::max(config.camera.imageHeight, 1);
  const int samplesPerPixel = std::max(config.camera.samplesPerPixel, 1);

  ResolvedOutputPaths outputPaths{};
  if (!resolveOutputPaths(args,
                          width,
                          height,
                          samplesPerPixel,
                          config.camera.maxDepth,
                          outputPaths,
                          errorMessage)) {
    std::cerr << errorMessage << '\n';
    return 1;
  }

  const torirender::Camera camera(config.camera.lookFrom,
                                  config.camera.lookAt,
                                  config.camera.viewUp,
                                  config.camera.vfov,
                                  static_cast<double>(width) / static_cast<double>(height),
                                  width,
                                  height);

  const torirender::Scene scene(config);
  torirender::Image image(width, height);
  printRenderConfiguration(args, outputPaths, config, width, height, samplesPerPixel);
  const auto renderStart = std::chrono::steady_clock::now();

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      torirender::Vec3 accumulated{};

      for (int sample = 0; sample < samplesPerPixel; ++sample) {
        torirender::Ray ray;
        if (samplesPerPixel == 1) {
          ray = camera.getRay(x, y);
        } else {
          std::uint64_t sampleState = config.camera.rngSeed;
          sampleState ^= static_cast<std::uint64_t>((y * width) + x) * 0x9E3779B97F4A7C15ULL;
          sampleState ^= static_cast<std::uint64_t>(sample) * 0xBF58476D1CE4E5B9ULL;
          const double jitterX = randomUnit(sampleState) - 0.5;
          const double jitterY = randomUnit(sampleState) - 0.5;
          ray = camera.getRay(static_cast<double>(x) + jitterX, static_cast<double>(y) + jitterY);
        }

        accumulated += scene.trace(ray, config.camera.maxDepth);
      }

      const torirender::Vec3 color = accumulated / static_cast<double>(samplesPerPixel);
      image.setPixel(x, y, color);
    }
  }

  if (!image.save(outputPaths.imagePath.string())) {
    std::cerr << "Failed to save output image: " << outputPaths.imagePath.string() << '\n';
    return 1;
  }

  const auto renderEnd = std::chrono::steady_clock::now();
  const std::chrono::duration<double> elapsed = renderEnd - renderStart;
  if (!appendRenderMetricsCsv(outputPaths.metricsCsvPath,
                              outputPaths.imageFileName,
                              width,
                              height,
                              samplesPerPixel,
                              config.camera.maxDepth,
                              elapsed.count(),
                              errorMessage)) {
    std::cerr << "Warning: " << errorMessage << '\n';
  }

  printRenderCompleted(outputPaths.imagePath.string(),
                       outputPaths.metricsCsvPath.string(),
                       elapsed.count(),
                       width,
                       height,
                       samplesPerPixel,
                       config.camera.maxDepth);
  return 0;
}
