// ONNX Runtime CPU backend (same session options as detection.detect_file).
#include <stdexcept>
#include <string>

#include "hm/vision/detector.hpp"

#ifdef HM_VISION_HAVE_ORT
#include <onnxruntime_cxx_api.h>
#endif

namespace hm::vision {

#ifdef HM_VISION_HAVE_ORT
namespace {

Ort::Env& ort_env() {
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "hm_vision");
  return env;
}

class OrtBackend final : public InferenceBackend {
 public:
  OrtBackend(const std::filesystem::path& model_path, int threads) : path_(model_path) {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads);
    options.SetInterOpNumThreads(1);
    session_ = std::make_unique<Ort::Session>(ort_env(), model_path.c_str(), options);
    Ort::AllocatorWithDefaultOptions allocator;
    input_name_ = session_->GetInputNameAllocated(0, allocator).get();
    output_name_ = session_->GetOutputNameAllocated(0, allocator).get();
    shape_ = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    for (auto dim : shape_) {
      if (dim <= 0) throw std::invalid_argument(model_path.string() + ": fixed NCHW input required");
    }
  }

  std::vector<std::int64_t> input_shape() const override { return shape_; }
  std::string name() const override { return "onnxruntime-cpu"; }

  std::vector<float> run(const std::vector<float>& input, std::vector<std::int64_t>* output_shape) override {
    std::size_t expected = 1;
    for (auto dim : shape_) expected *= static_cast<std::size_t>(dim);
    if (input.size() != expected) throw std::invalid_argument("input tensor size does not match the model");
    const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data()), input.size(),
                                                        shape_.data(), shape_.size());
    const char* inputs[] = {input_name_.c_str()};
    const char* outputs[] = {output_name_.c_str()};
    auto values = session_->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    auto info = values[0].GetTensorTypeAndShapeInfo();
    if (output_shape) *output_shape = info.GetShape();
    const float* data = values[0].GetTensorData<float>();
    return std::vector<float>(data, data + info.GetElementCount());
  }

 private:
  std::filesystem::path path_;
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_;
  std::string output_name_;
  std::vector<std::int64_t> shape_;
};

}  // namespace

bool onnxruntime_available() { return true; }

std::unique_ptr<InferenceBackend> make_onnxruntime_backend(const std::filesystem::path& model_path, int threads) {
  try {
    return std::make_unique<OrtBackend>(model_path, threads);
  } catch (const Ort::Exception& error) {
    throw std::runtime_error(model_path.string() + ": " + error.what());
  }
}
#else
bool onnxruntime_available() { return false; }

std::unique_ptr<InferenceBackend> make_onnxruntime_backend(const std::filesystem::path& model_path, int) {
  throw std::runtime_error("hm_vision was built without ONNX Runtime (HM_ONNXRUNTIME_ROOT); cannot load " +
                           model_path.string());
}
#endif

}  // namespace hm::vision
