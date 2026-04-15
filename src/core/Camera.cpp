#include "core/Camera.hpp"

#include <cmath>

#include "math/Utils.hpp"

namespace torirender {

namespace {

double sanitizeAspectRatio(double aspectRatio) noexcept {
  return aspectRatio > math::kEpsilon ? aspectRatio : 1.0;  // avoid invalid ratio
}

double sanitizeFovDegrees(double verticalFovDegrees) noexcept {
  return math::clamp(verticalFovDegrees, 1.0, 179.0);  // valid FOV range
}

int sanitizeImageDimension(int value) noexcept {
  return value > 0 ? value : 1;
}  // >=1

int deriveImageHeight(int imageWidth, double aspectRatio) noexcept {
  if (aspectRatio <= math::kEpsilon) {
    return imageWidth;  // fallback square
  }
  const int height = static_cast<int>(static_cast<double>(imageWidth) / aspectRatio);
  return height > 0 ? height : 1;
}

Vec3 fallbackUpFor(const Vec3& w) noexcept {
  if (std::fabs(w.y) < 0.999) {
    return Vec3(0.0, 1.0, 0.0);  // default up (landscape)
  }
  return Vec3(1.0, 0.0, 0.0);  // alternate up
}

TORIRENDER_ACC_ROUTINE_SEQ
double clampForRay(double value, double lo, double hi) noexcept {
  return value < lo ? lo : (value > hi ? hi : value);
}

}  // namespace

Camera::Camera() noexcept
    : Camera(Vec3(0.0, 0.0, 0.0), Vec3(0.0, 0.0, -1.0), Vec3(0.0, 1.0, 0.0), 90.0, 1.0, 1, 1) {
}  // testing camera

// Actual camera
Camera::Camera(const Vec3& lookFrom,
               const Vec3& lookAt,
               const Vec3& viewUp,
               double verticalFovDegrees,
               double aspectRatio,
               int imageWidth,
               int imageHeight) noexcept {
  const double aspect = sanitizeAspectRatio(aspectRatio);
  const double vfov = sanitizeFovDegrees(verticalFovDegrees);
  imageWidth_ = sanitizeImageDimension(imageWidth);
  imageHeight_ = imageHeight > 0 ? imageHeight : deriveImageHeight(imageWidth_, aspect);

  Vec3 lookDirection = lookFrom - lookAt;
  double focalDistance = lookDirection.length();
  if (focalDistance <= math::kEpsilon) {
    lookDirection = Vec3(0.0, 0.0, 1.0);
    focalDistance = 1.0;  // fallback distance
  }

  // camera defaults
  origin_ = lookFrom;
  w_ = lookDirection / focalDistance;  // backward

  Vec3 up = viewUp.nearZero() ? Vec3(0.0, 1.0, 0.0) : viewUp.normalized();

  // right vector
  u_ = cross(up, w_);
  if (u_.nearZero()) {
    up = fallbackUpFor(w_);
    u_ = cross(up, w_);
  }
  u_.normalize();

  // vertical vector to top
  v_ = cross(w_, u_);

  const double theta = math::degreesToRadians(vfov);
  const double h = std::tan(theta * 0.5);
  const double viewportHeight = 2.0 * h * focalDistance;
  const double viewportWidth = viewportHeight * aspect;

  // vector from left edge to right edge of image plane
  const Vec3 viewportU = viewportWidth * u_;

  // vector from top edge to bottom edge of image plane
  const Vec3 viewportV = -viewportHeight * v_;

  // How much to move in 3D space when you go from one pixel to the next?
  pixelDeltaU_ = viewportU / static_cast<double>(imageWidth_);
  pixelDeltaV_ = viewportV / static_cast<double>(imageHeight_);

  // going front of camera by focalDistance, then Top-left corner of the image
  // plane in world space
  const Vec3 viewportUpperLeft =
      origin_ - (focalDistance * w_) - (viewportU * 0.5) - (viewportV * 0.5);

  pixel00Loc_ =
      viewportUpperLeft + 0.5 * (pixelDeltaU_ + pixelDeltaV_);  // center of the top-left pixel cell
}

TORIRENDER_ACC_ROUTINE_SEQ
int Camera::imageWidth() const noexcept {
  return imageWidth_;
}

TORIRENDER_ACC_ROUTINE_SEQ
int Camera::imageHeight() const noexcept {
  return imageHeight_;
}

TORIRENDER_ACC_ROUTINE_SEQ
Ray Camera::getRay(double pixelX, double pixelY) const noexcept {
  const double i = clampForRay(pixelX, 0.0, static_cast<double>(imageWidth_ - 1));
  const double j = clampForRay(pixelY, 0.0, static_cast<double>(imageHeight_ - 1));

  const Vec3 pixelCenter = pixel00Loc_ + (i * pixelDeltaU_) + (j * pixelDeltaV_);

  Vec3 direction = pixelCenter - origin_;
  if (direction.nearZero()) {
    direction = -w_;
  }

  return Ray(origin_, direction.normalized());
}

TORIRENDER_ACC_ROUTINE_SEQ
Ray Camera::getRay(int pixelX, int pixelY) const noexcept {
  return getRay(static_cast<double>(pixelX), static_cast<double>(pixelY));
}

}  // namespace torirender
