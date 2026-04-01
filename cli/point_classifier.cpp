#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

#include "geometry/Torus.hpp"

namespace {

bool parseDouble(const char* token, double& outValue) {
  if (token == nullptr) {
    return false;
  }

  char* end = nullptr;
  outValue = std::strtod(token, &end);
  return end != token && end != nullptr && *end == '\0';
}

std::optional<torirender::TorusAxis> parseAxis(const std::string& axisToken) {
  std::string lower = axisToken;
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  if (lower == "x") {
    return torirender::TorusAxis::X;
  }
  if (lower == "y") {
    return torirender::TorusAxis::Y;
  }
  if (lower == "z") {
    return torirender::TorusAxis::Z;
  }

  return std::nullopt;
}

void printUsage() {
  std::cout << "Usage: point_classifier <x> <y> <z> <R> <r> [axis]\n"
               "  axis options: x, y, z (default: y)\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6 || argc > 7) {
    printUsage();
    return 1;
  }

  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double majorRadius = 0.0;
  double minorRadius = 0.0;

  if (!parseDouble(argv[1], x) || !parseDouble(argv[2], y) || !parseDouble(argv[3], z) ||
      !parseDouble(argv[4], majorRadius) || !parseDouble(argv[5], minorRadius)) {
    std::cerr << "Failed to parse numeric input arguments.\n";
    printUsage();
    return 1;
  }

  torirender::TorusAxis axis = torirender::TorusAxis::Y;
  if (argc == 7) {
    const auto parsedAxis = parseAxis(argv[6]);
    if (!parsedAxis.has_value()) {
      std::cerr << "Invalid axis. Expected x, y, or z.\n";
      printUsage();
      return 1;
    }
    axis = *parsedAxis;
  }

  const torirender::Torus torus(majorRadius, minorRadius, torirender::Vec3{}, axis);
  const torirender::Vec3 point(x, y, z);

  const double value = torus.evaluate(point);
  const torirender::PointClassification classification = torus.classify(point);

  std::cout << std::setprecision(15);
  std::cout << "F(P) = " << value << '\n';
  std::cout << "Classification = " << torirender::toString(classification) << '\n';

  return 0;
}
