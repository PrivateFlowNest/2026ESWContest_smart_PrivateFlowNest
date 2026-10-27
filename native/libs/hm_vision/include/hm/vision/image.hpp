// 8-bit BGR images and the OpenCV-compatible pixel operations the YOLOX path needs.
// Every function here is a bit-exact port of what services/edge-ai/vision-presence does with cv2
// (imread IMREAD_COLOR, resize INTER_LINEAR, cvtColor YUV2BGR_YUYV), so the model sees the same tensor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace hm::vision {

// Row-major HWC, tightly packed, channel order B, G, R (OpenCV CV_8UC3).
struct Image {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> bgr;

  bool empty() const { return width <= 0 || height <= 0; }
  std::uint8_t* row(int y) { return bgr.data() + static_cast<std::size_t>(y) * width * 3; }
  const std::uint8_t* row(int y) const { return bgr.data() + static_cast<std::size_t>(y) * width * 3; }
};

Image make_image(int width, int height, std::uint8_t fill = 0);

// cv2.imread(path, cv2.IMREAD_COLOR) for JPEG (libjpeg-turbo, EXIF orientation applied like OpenCV) and PNG.
// Throws std::runtime_error for unreadable or unsupported files.
Image read_image(const std::filesystem::path& path);
Image decode_image(const std::uint8_t* data, std::size_t size);  // sniffs JPEG / PNG magic
Image decode_jpeg(const std::uint8_t* data, std::size_t size, bool apply_exif_orientation = true);
Image decode_png(const std::uint8_t* data, std::size_t size);

// 8-bit RGB PNG (libpng). Writes PATH.tmp and renames it, so a reader never sees a half-written file.
void write_png(const Image& image, const std::filesystem::path& path);

// cv2.cvtColor(yuyv, cv2.COLOR_YUV2BGR_YUYV); stride in bytes (>= width * 2), width must be even.
Image yuyv_to_bgr(const std::uint8_t* data, int width, int height, std::size_t stride);

// cv2.resize(src, (width, height), interpolation=cv2.INTER_LINEAR) for CV_8UC3, including OpenCV's
// fixed-point coefficients and its INTER_AREA shortcut for an exact 2x downscale.
Image resize_linear(const Image& src, int width, int height);

// OpenCV ExifTransform for orientation 1..8 (flip / transpose); other values leave the image unchanged.
Image apply_orientation(const Image& src, int orientation);

}  // namespace hm::vision
