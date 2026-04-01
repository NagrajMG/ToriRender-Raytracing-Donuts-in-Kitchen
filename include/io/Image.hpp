#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "math/Vec3.hpp"

namespace torirender {

class Image {
 public:
  Image(int width, int height);  // constructor for image buffer

  int width() const noexcept;   // get width
  int height() const noexcept;  // get height

  void clear(const Vec3& color) noexcept;  // fill entire image

  void setPixel(int x, int y, const Vec3& color) noexcept;  // write pixel
  Vec3 getPixel(int x, int y) const noexcept;               // read pixel

  bool savePPM(const std::string& filePath) const;  // save as .ppm
  bool savePNG(const std::string& filePath) const;  // save as .png
  bool save(const std::string& filePath) const;

 private:
  std::size_t indexOf(int x, int y) const noexcept;      // (x,y) → index (1D)
  static unsigned char toByte(double channel) noexcept;  // 8 bits images

  int width_;
  int height_;
  std::vector<Vec3> pixels_;  // pixel buffer as rgb with Vec3
};

}  // namespace torirender