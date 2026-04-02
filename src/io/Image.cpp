#include "io/Image.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "math/Utils.hpp"

// Enable PNG support if stb_image_write is available
// TORIRENDER_HAS_STB_IMAGE_WRITE is expected to be defined via CMake
#if defined(TORIRENDER_HAS_STB_IMAGE_WRITE)
#if __has_include("stb_image_write.h")
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define TORIRENDER_IMAGE_HAS_STB 1
#elif __has_include(<stb_image_write.h>)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define TORIRENDER_IMAGE_HAS_STB 1
#elif __has_include(<stb/stb_image_write.h>)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
#define TORIRENDER_IMAGE_HAS_STB 1
#else
#define TORIRENDER_IMAGE_HAS_STB 0
#endif
#else
#define TORIRENDER_IMAGE_HAS_STB 0
#endif

namespace torirender {

namespace {

double toDisplaySpace(double channel) noexcept {
  const double clampedLinear = std::max(channel, 0.0);
  const double toneMapped = clampedLinear / (1.0 + clampedLinear);
  return std::pow(toneMapped, 1.0 / 2.2);
}

}  // namespace

// Construct image with given dimensions and allocate pixel buffer
Image::Image(int width, int height) : width_(width), height_(height), pixels_() {
  if (width <= 0 || height <= 0) {
    throw std::invalid_argument("Image dimensions must be positive");
  }

  // Allocate width * height pixels, initialized to (0,0,0)
  pixels_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), Vec3{});
}

// Get image width
int Image::width() const noexcept {
  return width_;
}

// Get image height
int Image::height() const noexcept {
  return height_;
}

// Fill entire image with a single color
void Image::clear(const Vec3& color) noexcept {
  std::fill(pixels_.begin(), pixels_.end(), color);
}

// Convert 2D pixel coordinates (x,y) to 1D index (row-major order)
std::size_t Image::indexOf(int x, int y) const noexcept {
  return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
         static_cast<std::size_t>(x);
}

// Set pixel color if coordinates are valid
void Image::setPixel(int x, int y, const Vec3& color) noexcept {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) {
    return;  // ignore out-of-bounds
  }
  pixels_[indexOf(x, y)] = color;
}

// Get pixel color if valid, otherwise return black
Vec3 Image::getPixel(int x, int y) const noexcept {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) {
    return Vec3{};
  }
  return pixels_[indexOf(x, y)];
}

// Convert floating-point color [0,1] to byte [0,255]
unsigned char Image::toByte(double channel) noexcept {
  const double clamped = math::clamp(toDisplaySpace(channel), 0.0, 1.0);  // avoid overflow
  const double scaled = clamped * 255.0;                                  // scale to 8-bit range
  return static_cast<unsigned char>(scaled + 0.5);                        // round to nearest
}

// Save image as binary PPM
bool Image::savePPM(const std::string& filePath) const {
  std::ofstream output(filePath, std::ios::binary);
  if (!output) {
    return false;  // failed to open file
  }

  // PPM header (P6 = binary RGB)
  output << "P6\n" << width_ << ' ' << height_ << "\n255\n";

  // Write all pixels sequentially (row-major)
  for (const Vec3& pixel : pixels_) {
    const unsigned char rgb[3] = {
        toByte(pixel.x),
        toByte(pixel.y),
        toByte(pixel.z),
    };
    output.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
  }

  return static_cast<bool>(output);
}

// Save image as PNG using stb (if available)
bool Image::savePNG(const std::string& filePath) const {
// for png file
#if TORIRENDER_IMAGE_HAS_STB
  // Pack Vec3 pixels into contiguous RGB byte buffer
  std::vector<unsigned char> packed;
  packed.resize(pixels_.size() * 3U);

  for (std::size_t i = 0; i < pixels_.size(); ++i) {
    packed[(i * 3U) + 0U] = toByte(pixels_[i].x);
    packed[(i * 3U) + 1U] = toByte(pixels_[i].y);
    packed[(i * 3U) + 2U] = toByte(pixels_[i].z);
  }

  // Write PNG (stride = width * 3 bytes)
  return stbi_write_png(filePath.c_str(), width_, height_, 3, packed.data(), width_ * 3) != 0;
#else
  (void)filePath;  // suppress unused warning
  return false;    // PNG not supported
#endif
}

// Save image based on file extension
bool Image::save(const std::string& filePath) const {
  const std::size_t extensionPos = filePath.find_last_of('.');
  if (extensionPos == std::string::npos) {
    return false;  // no extension
  }

  // Convert extension to lowercase for comparison
  std::string extension = filePath.substr(extensionPos);
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  // Dispatch to appropriate format
  if (extension == ".ppm") {
    return savePPM(filePath);
  }
  if (extension == ".png") {
    return savePNG(filePath);
  }

  return false;
}

}  // namespace torirender