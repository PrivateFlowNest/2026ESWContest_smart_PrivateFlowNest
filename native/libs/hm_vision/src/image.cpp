#include "hm/vision/image.hpp"

#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifdef HM_VISION_HAVE_JPEG
#include <jpeglib.h>
#endif
#ifdef HM_VISION_HAVE_PNG
#include <png.h>
#endif

namespace hm::vision {

Image make_image(int width, int height, std::uint8_t fill) {
  if (width < 0 || height < 0) throw std::invalid_argument("negative image size");
  Image image;
  image.width = width;
  image.height = height;
  image.bgr.assign(static_cast<std::size_t>(width) * height * 3, fill);
  return image;
}

// ---------------------------------------------------------------------------------------------- EXIF

namespace {

// Orientation tag (0x0112) of IFD0 in an APP1 "Exif\0\0" payload; 1 when absent or malformed (OpenCV default).
[[maybe_unused]] int exif_orientation(const std::uint8_t* data, std::size_t size) {  // used by the JPEG path
  if (size < 14 || std::memcmp(data, "Exif\0\0", 6) != 0) return 1;
  const std::uint8_t* tiff = data + 6;
  const std::size_t length = size - 6;
  bool little;
  if (tiff[0] == 'I' && tiff[1] == 'I') {
    little = true;
  } else if (tiff[0] == 'M' && tiff[1] == 'M') {
    little = false;
  } else {
    return 1;
  }
  auto u16 = [&](std::size_t at) -> unsigned {
    return little ? unsigned(tiff[at]) | (unsigned(tiff[at + 1]) << 8) : (unsigned(tiff[at]) << 8) | tiff[at + 1];
  };
  auto u32 = [&](std::size_t at) -> std::uint32_t {
    return little ? std::uint32_t(u16(at)) | (std::uint32_t(u16(at + 2)) << 16)
                  : (std::uint32_t(u16(at)) << 16) | u16(at + 2);
  };
  if (u16(2) != 42) return 1;
  const std::uint32_t ifd = u32(4);
  if (ifd + 2 > length) return 1;
  const unsigned entries = u16(ifd);
  for (unsigned i = 0; i < entries; ++i) {
    const std::size_t entry = ifd + 2 + std::size_t(i) * 12;
    if (entry + 12 > length) return 1;
    if (u16(entry) == 0x0112 && u16(entry + 2) == 3) return static_cast<int>(u16(entry + 8));
  }
  return 1;
}

}  // namespace

Image apply_orientation(const Image& src, int orientation) {
  if (orientation < 2 || orientation > 8) return src;
  const bool transpose = orientation >= 5;
  const int width = transpose ? src.height : src.width;
  const int height = transpose ? src.width : src.height;
  // flip codes after the optional transpose: 2,6 -> horizontal; 3,7 -> both; 4,8 -> vertical
  const bool flip_h = orientation == 2 || orientation == 3 || orientation == 6 || orientation == 7;
  const bool flip_v = orientation == 3 || orientation == 4 || orientation == 7 || orientation == 8;
  Image out = make_image(width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int ty = flip_v ? height - 1 - y : y;  // position in the transposed (or plain) image
      const int tx = flip_h ? width - 1 - x : x;
      const int sy = transpose ? tx : ty;
      const int sx = transpose ? ty : tx;
      std::memcpy(out.row(y) + x * 3, src.row(sy) + sx * 3, 3);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------- JPEG

#ifdef HM_VISION_HAVE_JPEG
namespace {

struct JpegError {
  jpeg_error_mgr manager;
  std::jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};

void jpeg_error_exit(j_common_ptr info) {
  auto* error = reinterpret_cast<JpegError*>(info->err);
  (*info->err->format_message)(info, error->message);
  std::longjmp(error->jump, 1);
}

void jpeg_silent(j_common_ptr, int) {}

// libjpeg reports errors with longjmp: keep every C++ object out of this frame (out lives in the caller).
bool jpeg_decode_into(const std::uint8_t* data, std::size_t size, Image* out, int* orientation, char* message) {
  jpeg_decompress_struct info;
  JpegError error;
  info.err = jpeg_std_error(&error.manager);
  error.manager.error_exit = jpeg_error_exit;
  error.manager.emit_message = jpeg_silent;
  error.message[0] = '\0';
  if (setjmp(error.jump)) {
    std::snprintf(message, JMSG_LENGTH_MAX, "%s", error.message);
    jpeg_destroy_decompress(&info);
    return false;
  }
  jpeg_create_decompress(&info);
  jpeg_mem_src(&info, data, static_cast<unsigned long>(size));
  jpeg_save_markers(&info, JPEG_APP0 + 1, 0xFFFF);
  jpeg_read_header(&info, TRUE);
  if (info.num_components == 4) {
    std::snprintf(message, JMSG_LENGTH_MAX, "CMYK/YCCK JPEG is not supported");
    jpeg_destroy_decompress(&info);
    return false;
  }
  *orientation = 1;
  for (jpeg_saved_marker_ptr marker = info.marker_list; marker; marker = marker->next) {
    if (marker->marker == JPEG_APP0 + 1 && marker->data_length >= 6 &&
        std::memcmp(marker->data, "Exif\0\0", 6) == 0) {
      *orientation = exif_orientation(marker->data, marker->data_length);
      break;
    }
  }
  // Same decoder settings as OpenCV's JpegDecoder: libjpeg defaults (ISLOW IDCT, fancy upsampling), BGR out.
#ifdef JCS_EXTENSIONS
  info.out_color_space = JCS_EXT_BGR;
#else
  info.out_color_space = JCS_RGB;  // plain libjpeg: swap to BGR below (same values)
#endif
  info.out_color_components = 3;
  jpeg_start_decompress(&info);
  out->width = static_cast<int>(info.output_width);
  out->height = static_cast<int>(info.output_height);
  out->bgr.resize(static_cast<std::size_t>(out->width) * out->height * 3);
  while (info.output_scanline < info.output_height) {
    JSAMPROW row = out->row(static_cast<int>(info.output_scanline));
    jpeg_read_scanlines(&info, &row, 1);
#ifndef JCS_EXTENSIONS
    for (int x = 0; x < out->width; ++x) std::swap(row[3 * x], row[3 * x + 2]);
#endif
  }
  jpeg_finish_decompress(&info);
  jpeg_destroy_decompress(&info);
  return true;
}

}  // namespace
#endif

Image decode_jpeg(const std::uint8_t* data, std::size_t size, bool apply_exif_orientation) {
#ifdef HM_VISION_HAVE_JPEG
  Image image;
  int orientation = 1;
  char message[JMSG_LENGTH_MAX] = {0};
  if (!jpeg_decode_into(data, size, &image, &orientation, message)) {
    throw std::runtime_error(std::string("JPEG decode failed: ") + message);
  }
  return apply_exif_orientation ? apply_orientation(image, orientation) : image;
#else
  (void)data;
  (void)size;
  (void)apply_exif_orientation;
  throw std::runtime_error("hm_vision was built without libjpeg (install libjpeg-turbo8-dev)");
#endif
}

// ----------------------------------------------------------------------------------------------- PNG

#ifdef HM_VISION_HAVE_PNG
namespace {

struct PngInput {
  const std::uint8_t* data;
  std::size_t size;
  std::size_t position;
};

void png_read_memory(png_structp png, png_bytep target, png_size_t count) {
  auto* input = static_cast<PngInput*>(png_get_io_ptr(png));
  if (input->position + count > input->size) png_error(png, "truncated PNG");
  std::memcpy(target, input->data + input->position, count);
  input->position += count;
}

void png_error_message(png_structp png, png_const_charp text) {
  auto* message = static_cast<char*>(png_get_error_ptr(png));
  std::snprintf(message, 256, "%s", text);
  png_longjmp(png, 1);
}

void png_warning_silent(png_structp, png_const_charp) {}

// Same transformations as OpenCV's PngDecoder::readData for an 8-bit 3-channel target. libpng reports
// errors with longjmp, so the output image and row pointers are owned by the caller.
bool png_decode_into(const std::uint8_t* data, std::size_t size, Image* out, std::vector<png_bytep>* rows,
                     char* message) {
  PngInput input{data, size, 0};
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, message, png_error_message, png_warning_silent);
  if (!png) {
    std::snprintf(message, 256, "png_create_read_struct failed");
    return false;
  }
  png_infop info = png_create_info_struct(png);
  png_infop end_info = png_create_info_struct(png);
  if (!info || !end_info || setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, info ? &info : nullptr, end_info ? &end_info : nullptr);
    return false;
  }
  png_set_read_fn(png, &input, png_read_memory);
  png_read_info(png, info);
  const int bit_depth = png_get_bit_depth(png, info);
  const int color_type = png_get_color_type(png, info);
  if (bit_depth == 16) png_set_strip_16(png);
  png_set_strip_alpha(png);
  if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if ((color_type & PNG_COLOR_MASK_COLOR) == 0 && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (color_type & PNG_COLOR_MASK_COLOR) {
    png_set_bgr(png);
  } else {
    png_set_gray_to_rgb(png);
  }
  png_set_interlace_handling(png);
  png_read_update_info(png, info);
  out->width = static_cast<int>(png_get_image_width(png, info));
  out->height = static_cast<int>(png_get_image_height(png, info));
  if (png_get_rowbytes(png, info) != static_cast<png_size_t>(out->width) * 3) png_error(png, "unexpected row size");
  out->bgr.resize(static_cast<std::size_t>(out->width) * out->height * 3);
  rows->resize(static_cast<std::size_t>(out->height));
  for (int y = 0; y < out->height; ++y) (*rows)[static_cast<std::size_t>(y)] = out->row(y);
  png_read_image(png, rows->data());
  png_read_end(png, end_info);
  png_destroy_read_struct(&png, &info, &end_info);
  return true;
}

// 8-bit RGB from BGR rows (row pointers owned by the caller, see png_decode_into).
bool png_encode_into(std::FILE* file, const Image& image, std::vector<png_bytep>* rows, char* message) {
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, message, png_error_message, png_warning_silent);
  if (!png) {
    std::snprintf(message, 256, "png_create_write_struct failed");
    return false;
  }
  png_infop info = png_create_info_struct(png);
  if (!info || setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, info ? &info : nullptr);
    return false;
  }
  png_init_io(png, file);
  png_set_IHDR(png, info, static_cast<png_uint_32>(image.width), static_cast<png_uint_32>(image.height), 8,
               PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  png_set_bgr(png);
  png_write_image(png, rows->data());
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return true;
}

}  // namespace
#endif

Image decode_png(const std::uint8_t* data, std::size_t size) {
#ifdef HM_VISION_HAVE_PNG
  Image image;
  std::vector<png_bytep> rows;
  char message[256] = {0};
  if (!png_decode_into(data, size, &image, &rows, message)) {
    throw std::runtime_error(std::string("PNG decode failed: ") + message);
  }
  return image;
#else
  (void)data;
  (void)size;
  throw std::runtime_error("hm_vision was built without libpng (install libpng-dev)");
#endif
}

Image decode_image(const std::uint8_t* data, std::size_t size) {
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return decode_jpeg(data, size);
  if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return decode_png(data, size);
  throw std::runtime_error("unsupported image format (JPEG and PNG only)");
}

Image read_image(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("could not read image: " + path.string());
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  try {
    return decode_image(bytes.data(), bytes.size());
  } catch (const std::exception& error) {
    throw std::runtime_error(path.string() + ": " + error.what());
  }
}

void write_png(const Image& image, const std::filesystem::path& path) {
  if (image.empty() || image.bgr.size() != static_cast<std::size_t>(image.width) * image.height * 3) {
    throw std::invalid_argument("write_png: empty or inconsistent image");
  }
#ifdef HM_VISION_HAVE_PNG
  const std::filesystem::path temporary = path.string() + ".tmp";
  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) throw std::runtime_error("could not write " + temporary.string() + ": " + std::strerror(errno));
  std::vector<png_bytep> rows(static_cast<std::size_t>(image.height));
  for (int y = 0; y < image.height; ++y) rows[static_cast<std::size_t>(y)] = const_cast<png_bytep>(image.row(y));
  char message[256] = {0};
  const bool encoded = png_encode_into(file, image, &rows, message);
  const bool closed = std::fclose(file) == 0;
  if (!encoded || !closed) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("PNG encode failed for " + path.string() +
                             (message[0] ? std::string(": ") + message : ""));
  }
  std::filesystem::rename(temporary, path);
#else
  (void)path;
  throw std::runtime_error("hm_vision was built without libpng (install libpng-dev)");
#endif
}

// ---------------------------------------------------------------------------------------------- YUYV

namespace {

// OpenCV color_yuv: ITU-R BT.601 fixed point, 20-bit shift.
constexpr int kCY = 1220542, kCUB = 2116026, kCUG = -409993, kCVG = -852492, kCVR = 1673527, kShift = 20;

inline std::uint8_t sat_u8(int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); }

}  // namespace

Image yuyv_to_bgr(const std::uint8_t* data, int width, int height, std::size_t stride) {
  if (width <= 0 || height <= 0 || width % 2 != 0 || stride < static_cast<std::size_t>(width) * 2) {
    throw std::invalid_argument("YUYV needs an even width and stride >= 2 * width");
  }
  Image out = make_image(width, height);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* src = data + static_cast<std::size_t>(y) * stride;
    std::uint8_t* dst = out.row(y);
    for (int i = 0; i < 2 * width; i += 4, dst += 6) {
      const int u = int(src[i + 1]) - 128, v = int(src[i + 3]) - 128;
      const int ruv = (1 << (kShift - 1)) + kCVR * v;
      const int guv = (1 << (kShift - 1)) + kCVG * v + kCUG * u;
      const int buv = (1 << (kShift - 1)) + kCUB * u;
      for (int k = 0; k < 2; ++k) {
        const int yy = std::max(0, int(src[i + 2 * k]) - 16) * kCY;
        dst[3 * k + 2] = sat_u8((yy + ruv) >> kShift);
        dst[3 * k + 1] = sat_u8((yy + guv) >> kShift);
        dst[3 * k + 0] = sat_u8((yy + buv) >> kShift);
      }
    }
  }
  return out;
}

// -------------------------------------------------------------------------------------------- resize

namespace {

constexpr int kCoefBits = 11;
constexpr int kCoefScale = 1 << kCoefBits;  // INTER_RESIZE_COEF_SCALE

inline int cv_floor(float value) {
  const int i = static_cast<int>(value);
  return i - (static_cast<float>(i) > value);
}

inline short coef(float value) {  // saturate_cast<short>(float): cvRound (nearest-even) then clamp
  const long rounded = std::lrint(value * static_cast<float>(kCoefScale));
  return static_cast<short>(std::clamp<long>(rounded, -32768, 32767));
}

Image resize_area_fast2(const Image& src, int width, int height) {
  Image out = make_image(width, height);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* s0 = src.row(2 * y);
    const std::uint8_t* s1 = src.row(2 * y + 1);
    std::uint8_t* d = out.row(y);
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        const int a = 6 * x + c;
        d[3 * x + c] = static_cast<std::uint8_t>((s0[a] + s0[a + 3] + s1[a] + s1[a + 3] + 2) >> 2);
      }
    }
  }
  return out;
}

}  // namespace

Image resize_linear(const Image& src, int width, int height) {
  if (src.empty() || width <= 0 || height <= 0) throw std::invalid_argument("resize_linear: empty size");
  if (width == src.width && height == src.height) return src;  // cv::resize copies
  const int sw = src.width, sh = src.height, cn = 3;
  const double inv_scale_x = static_cast<double>(width) / sw;
  const double inv_scale_y = static_cast<double>(height) / sh;
  const double scale_x = 1. / inv_scale_x, scale_y = 1. / inv_scale_y;
  const long iscale_x = std::lrint(scale_x), iscale_y = std::lrint(scale_y);
  const bool area_fast = std::abs(scale_x - static_cast<double>(iscale_x)) < DBL_EPSILON &&
                         std::abs(scale_y - static_cast<double>(iscale_y)) < DBL_EPSILON;
  if (area_fast && iscale_x == 2 && iscale_y == 2) return resize_area_fast2(src, width, height);

  // Horizontal coefficients (resize(): xofs, ialpha, xmin/xmax with border clamping).
  std::vector<int> xofs(width);
  std::vector<short> alpha(static_cast<std::size_t>(width) * 2);
  int xmax = width;
  for (int dx = 0; dx < width; ++dx) {
    float fx = static_cast<float>((dx + 0.5) * scale_x - 0.5);
    int sx = cv_floor(fx);
    fx -= static_cast<float>(sx);
    if (sx < 0) {
      fx = 0.f;
      sx = 0;
    }
    if (sx + 1 >= sw) {
      xmax = std::min(xmax, dx);
      if (sx >= sw - 1) {
        fx = 0.f;
        sx = sw - 1;
      }
    }
    xofs[dx] = sx * cn;
    alpha[2 * dx] = coef(1.f - fx);
    alpha[2 * dx + 1] = coef(fx);
  }
  // Vertical coefficients (rows are clamped later, weights are not).
  std::vector<int> yofs(height);
  std::vector<short> beta(static_cast<std::size_t>(height) * 2);
  for (int dy = 0; dy < height; ++dy) {
    float fy = static_cast<float>((dy + 0.5) * scale_y - 0.5);
    const int sy = cv_floor(fy);
    fy -= static_cast<float>(sy);
    yofs[dy] = sy;
    beta[2 * dy] = coef(1.f - fy);
    beta[2 * dy + 1] = coef(fy);
  }

  const int row_width = width * cn;
  auto horizontal = [&](int sy, std::vector<int>& buffer) {
    const std::uint8_t* s = src.row(sy);
    for (int dx = 0; dx < width; ++dx) {
      const int sx = xofs[dx];
      const int a0 = alpha[2 * dx], a1 = alpha[2 * dx + 1];
      for (int c = 0; c < cn; ++c) {
        buffer[dx * cn + c] = dx < xmax ? s[sx + c] * a0 + s[sx + c + cn] * a1 : s[sx + c] * kCoefScale;
      }
    }
  };
  Image out = make_image(width, height);
  std::vector<int> row0(row_width), row1(row_width);
  int cached0 = -1, cached1 = -1;
  for (int dy = 0; dy < height; ++dy) {
    const int r0 = std::clamp(yofs[dy], 0, sh - 1);
    const int r1 = std::clamp(yofs[dy] + 1, 0, sh - 1);
    if (r0 == cached1) {
      std::swap(row0, row1);
      std::swap(cached0, cached1);
    }
    if (r0 != cached0) {
      horizontal(r0, row0);
      cached0 = r0;
    }
    if (r1 != cached1) {
      horizontal(r1, row1);
      cached1 = r1;
    }
    const int b0 = beta[2 * dy], b1 = beta[2 * dy + 1];
    std::uint8_t* d = out.row(dy);
    // VResizeLinear<uchar, int, short, ...>: same expression as OpenCV's SIMD and scalar paths.
    for (int x = 0; x < row_width; ++x) {
      d[x] = static_cast<std::uint8_t>((((b0 * (row0[x] >> 4)) >> 16) + ((b1 * (row1[x] >> 4)) >> 16) + 2) >> 2);
    }
  }
  return out;
}

}  // namespace hm::vision
