#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include "geometry/Torus.hpp"
#include "scene/SceneConfig.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

// string to double parsing
bool parseDouble(const char* token, double& outValue) {
  if (token == nullptr)
    return false;

  char* end = nullptr;
  outValue = std::strtod(token, &end);
  return end != token && end != nullptr && *end == '\0';
}

// usage helper
void printUsage() {
  std::cout << "Usage: point_classifier <x> <y> <z>\n";
}

struct CliTheme {
  bool colorEnabled = false;
  const char* reset = "\033[0m";
  const char* title = "\033[1;33m";
  const char* section = "\033[1;34m";
  const char* key = "\033[1;37m";
  const char* value = "\033[1;32m";
  const char* success = "\033[1;32m";
  const char* warning = "\033[1;33m";
  const char* danger = "\033[1;31m";
  const char* border = "\033[1;34m";
};

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

void printKeyValue(const CliTheme& theme, const std::string& key, const std::string& value) {
  std::cout << "  " << styled(theme, theme.key, key + ": ") << styled(theme, theme.value, value)
            << '\n';
}

const char* classificationStyle(const CliTheme& theme,
                                torirender::PointClassification classification) {
  switch (classification) {
    case torirender::PointClassification::Inside:
      return theme.danger;
    case torirender::PointClassification::On:
      return theme.warning;
    case torirender::PointClassification::Outside:
      return theme.success;
  }
  return theme.value;
}

// parse axis string to enum
torirender::TorusAxis parseAxisToken(const std::string& token) {
  std::string lower = token;
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  if (lower == "x")
    return torirender::TorusAxis::X;
  if (lower == "z")
    return torirender::TorusAxis::Z;
  return torirender::TorusAxis::Y;
}

// build torus from config
torirender::Torus buildTorusFromConfig(const torirender::TorusConfig& config) {
  if (config.hasAxisDirection) {
    return torirender::Torus(
        config.majorRadius, config.minorRadius, config.center, config.axisDirection);
  }
  return torirender::Torus(
      config.majorRadius, config.minorRadius, config.center, parseAxisToken(config.axis));
}

}  // namespace

int main(int argc, char** argv) {
  // expect exactly 3 inputs
  if (argc != 4) {
    printUsage();
    return 1;
  }

  double x = 0.0, y = 0.0, z = 0.0;

  // parse point coordinates
  if (!parseDouble(argv[1], x) || !parseDouble(argv[2], y) || !parseDouble(argv[3], z)) {
    std::cerr << "Failed to parse numeric input arguments.\n";
    printUsage();
    return 1;
  }

  const torirender::Vec3 point(x, y, z);

  // load scene config (JSON)
  const std::string configPath = "config/scene.json";
  torirender::SceneConfig sceneConfig{};
  std::string errorMessage;

  if (!torirender::loadSceneConfigFromJsonFile(configPath, sceneConfig, &errorMessage)) {
    std::cerr << "Failed to load scene config from " << configPath << ": " << errorMessage << '\n';
    return 1;
  }

  // build two tori from config
  const torirender::Torus torus1 = buildTorusFromConfig(sceneConfig.torusPrimary);
  const torirender::Torus torus2 = buildTorusFromConfig(sceneConfig.torusSecondary);

  // evaluate implicit function
  const double f1 = torus1.evaluate(point);
  const double f2 = torus2.evaluate(point);

  // classify point w.r.t each torus
  const torirender::PointClassification class1 = torus1.classify(point);
  const torirender::PointClassification class2 = torus2.classify(point);

  // epsilon threshold for boundary checks
  constexpr double epsilon = 1e-8;

  const bool on1 = std::fabs(f1) <= epsilon;
  const bool on2 = std::fabs(f2) <= epsilon;
  const bool inside1 = f1 < -epsilon;
  const bool inside2 = f2 < -epsilon;
  const bool outside1 = f1 > epsilon;
  const bool outside2 = f2 > epsilon;

  // CLI color theme
  CliTheme theme{};
  theme.colorEnabled = supportsColor();

  // combined classification logic
  std::string combined;
  const char* combinedStyle = theme.value;
  if (on1 && on2) {
    combined = "On both tori";
    combinedStyle = theme.warning;
  } else if (inside1 && outside2) {
    combined = "Inside only torus1";
    combinedStyle = theme.warning;
  } else if (inside2 && outside1) {
    combined = "Inside only torus2";
    combinedStyle = theme.warning;
  } else if (outside1 && outside2) {
    combined = "Outside both";
    combinedStyle = theme.success;
  } else if (inside1 && inside2) {
    combined = "Inside both tori";
    combinedStyle = theme.danger;
  } else if (on1 && !on2) {
    combined = "On torus 1 surface";
    combinedStyle = theme.warning;
  } else if (!on1 && on2) {
    combined = "On torus 2 surface";
    combinedStyle = theme.warning;
  } else {
    combined = "Mixed boundary region";
    combinedStyle = theme.value;
  }

  // print results
  std::cout << std::setprecision(15) << std::fixed;
  std::cout << '\n';
  std::cout << styled(theme, theme.title, "ToriRender - Point Classifier") << '\n';
  std::cout << styled(theme,
                      theme.border,
                      "============================================================")
            << '\n';

  printKeyValue(
      theme,
      "Point",
      "(" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ")");

  std::cout << '\n' << styled(theme, theme.section, "Torus 1") << '\n';
  printKeyValue(theme, "F(P)", std::to_string(f1));
  std::cout << "  " << styled(theme, theme.key, "Classification: ")
            << styled(theme, classificationStyle(theme, class1), torirender::toString(class1))
            << "\n";

  std::cout << '\n' << styled(theme, theme.section, "Torus 2") << '\n';
  printKeyValue(theme, "F(P)", std::to_string(f2));
  std::cout << "  " << styled(theme, theme.key, "Classification: ")
            << styled(theme, classificationStyle(theme, class2), torirender::toString(class2))
            << "\n";

  std::cout << '\n' << styled(theme, theme.section, "Combined") << '\n';
  std::cout << "  " << styled(theme, theme.key, "Classification: ")
            << styled(theme, combinedStyle, combined) << '\n';

  std::cout << styled(theme,
                      theme.border,
                      "============================================================")
            << '\n';

  return 0;
}
