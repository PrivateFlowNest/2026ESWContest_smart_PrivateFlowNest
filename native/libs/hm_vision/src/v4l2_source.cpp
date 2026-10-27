// V4L2 mmap capture (PC/Linux webcam fallback). MJPEG frames are decoded like cv2.imdecode, YUYV like
// cv2.cvtColor(COLOR_YUV2BGR_YUYV), i.e. what OpenCV's cap_v4l backend does.
#include <algorithm>
#include <stdexcept>
#include <string>

#include "hm/vision/sources.hpp"

#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <vector>
#endif

namespace hm::vision {

#ifdef __linux__
namespace {

int xioctl(int fd, unsigned long request, void* argument) {
  int result;
  do {
    result = ioctl(fd, request, argument);
  } while (result == -1 && errno == EINTR);
  return result;
}

std::string errno_text(const std::string& what) { return what + ": " + std::strerror(errno); }

std::int64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

class V4l2Source final : public FrameSource {
 public:
  explicit V4l2Source(const V4l2Options& options) : options_(options) {
    if (options.pixel_format == "MJPG") {
      fourcc_ = V4L2_PIX_FMT_MJPEG;
    } else if (options.pixel_format == "YUYV") {
      fourcc_ = V4L2_PIX_FMT_YUYV;
    } else {
      throw std::invalid_argument("v4l2 pixel_format must be MJPG or YUYV");
    }
    fd_ = ::open(options.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error(errno_text("open " + options.device));
    try {
      configure();
    } catch (...) {
      release();
      throw;
    }
  }
  ~V4l2Source() override { release(); }

  std::optional<Frame> grab() override {
    pollfd waiter{fd_, POLLIN, 0};
    const int ready = poll(&waiter, 1, options_.timeout_ms);
    if (ready < 0) throw std::runtime_error(errno_text("poll " + options_.device));
    if (ready == 0) throw std::runtime_error(options_.device + ": no frame within timeout");
    // Drain every filled buffer and keep the newest one, so a 5 fps consumer never reads stale frames.
    v4l2_buffer newest{};
    bool have = false;
    for (;;) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) break;
        throw std::runtime_error(errno_text("VIDIOC_DQBUF"));
      }
      if (have) requeue(newest);
      newest = buffer;
      have = true;
    }
    if (!have) throw std::runtime_error(options_.device + ": poll reported data but no buffer was ready");
    Frame frame;
    try {
      const auto* data = static_cast<const std::uint8_t*>(buffers_[newest.index].start);
      if (fourcc_ == V4L2_PIX_FMT_MJPEG) {
        frame.image = decode_jpeg(data, newest.bytesused);
      } else {
        frame.image = yuyv_to_bgr(data, width_, height_, stride_);
      }
    } catch (...) {
      requeue(newest);
      throw;
    }
    std::int64_t age = 0;
    if ((newest.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
      const std::int64_t stamp = static_cast<std::int64_t>(newest.timestamp.tv_sec) * 1000 + newest.timestamp.tv_usec / 1000;
      age = std::max<std::int64_t>(0, monotonic_ms() - stamp);
    }
    frame.captured_at_ms = hm::now_ms() - age;
    requeue(newest);
    return frame;
  }

  bool live() const override { return true; }
  std::string describe() const override {
    return "v4l2:" + options_.device + " " + std::to_string(width_) + "x" + std::to_string(height_) + " " +
           options_.pixel_format;
  }

 private:
  struct Mapping {
    void* start = MAP_FAILED;
    std::size_t length = 0;
  };

  void configure() {
    v4l2_capability capability{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0) throw std::runtime_error(errno_text("VIDIOC_QUERYCAP"));
    const std::uint32_t caps =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
      throw std::runtime_error(options_.device + " is not a streaming capture device");
    }
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<std::uint32_t>(options_.width);
    format.fmt.pix.height = static_cast<std::uint32_t>(options_.height);
    format.fmt.pix.pixelformat = fourcc_;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) throw std::runtime_error(errno_text("VIDIOC_S_FMT"));
    if (format.fmt.pix.pixelformat != fourcc_) {
      throw std::runtime_error(options_.device + " does not support " + options_.pixel_format);
    }
    width_ = static_cast<int>(format.fmt.pix.width);
    height_ = static_cast<int>(format.fmt.pix.height);
    stride_ = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline : static_cast<std::size_t>(width_) * 2;
    if (options_.fps > 0) {
      v4l2_streamparm parm{};
      parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      parm.parm.capture.timeperframe.numerator = 1;
      parm.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(options_.fps);
      xioctl(fd_, VIDIOC_S_PARM, &parm);  // best effort: not every driver supports it
    }
    v4l2_requestbuffers request{};
    request.count = static_cast<std::uint32_t>(std::max(2, options_.buffers));
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0) throw std::runtime_error(errno_text("VIDIOC_REQBUFS"));
    if (request.count < 2) throw std::runtime_error(options_.device + ": not enough capture buffers");
    buffers_.resize(request.count);
    for (std::uint32_t i = 0; i < request.count; ++i) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = i;
      if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) throw std::runtime_error(errno_text("VIDIOC_QUERYBUF"));
      buffers_[i].length = buffer.length;
      buffers_[i].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
      if (buffers_[i].start == MAP_FAILED) throw std::runtime_error(errno_text("mmap"));
      if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) throw std::runtime_error(errno_text("VIDIOC_QBUF"));
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) throw std::runtime_error(errno_text("VIDIOC_STREAMON"));
    streaming_ = true;
  }

  void requeue(v4l2_buffer buffer) {
    if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) throw std::runtime_error(errno_text("VIDIOC_QBUF"));
  }

  void release() {
    if (fd_ < 0) return;
    if (streaming_) {
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    for (auto& mapping : buffers_) {
      if (mapping.start != MAP_FAILED) munmap(mapping.start, mapping.length);
    }
    buffers_.clear();
    ::close(fd_);
    fd_ = -1;
  }

  V4l2Options options_;
  std::uint32_t fourcc_ = 0;
  int fd_ = -1;
  int width_ = 0;
  int height_ = 0;
  std::size_t stride_ = 0;
  bool streaming_ = false;
  std::vector<Mapping> buffers_;
};

}  // namespace

bool v4l2_available() { return true; }
std::unique_ptr<FrameSource> make_v4l2_source(const V4l2Options& options) {
  return std::make_unique<V4l2Source>(options);
}
#else
bool v4l2_available() { return false; }
std::unique_ptr<FrameSource> make_v4l2_source(const V4l2Options&) {
  throw std::runtime_error("V4L2 capture is only available on Linux");
}
#endif

}  // namespace hm::vision
