#include "detector.hpp"

#include <stdexcept>

#ifdef FACESHIELD_HAVE_VITIS
#include <vitis/ai/facedetect.hpp>
#endif

namespace faceshield {

namespace {

class FullFrameDetector : public FaceDetector {
 public:
  cv::Size input_size() const override { return {64, 36}; }
  std::vector<Box> detect(const cv::Mat&, cv::Size full) override {
    return {Box{0, 0, static_cast<float>(full.width),
                static_cast<float>(full.height), 1.f}};
  }
};

#ifdef FACESHIELD_HAVE_VITIS
class VitisDetector : public FaceDetector {
 public:
  VitisDetector(const std::string& model, float threshold)
      : impl_(vitis::ai::FaceDetect::create(model, true)) {
    if (!impl_) throw std::runtime_error("could not load model " + model);
    impl_->setThreshold(threshold);
  }
  cv::Size input_size() const override {
    return {impl_->getInputWidth(), impl_->getInputHeight()};
  }
  std::vector<Box> detect(const cv::Mat& bgr, cv::Size full) override {
    // Rects come back normalised to [0, 1] of the input image.
    const auto result = impl_->run(bgr);
    std::vector<Box> boxes;
    boxes.reserve(result.rects.size());
    for (const auto& r : result.rects) {
      boxes.push_back(Box{r.x * full.width, r.y * full.height,
                          r.width * full.width, r.height * full.height,
                          r.score});
    }
    return boxes;
  }

 private:
  std::unique_ptr<vitis::ai::FaceDetect> impl_;
};
#endif

}  // namespace

std::unique_ptr<FaceDetector> make_vitis_detector(const std::string& model,
                                                  float threshold) {
#ifdef FACESHIELD_HAVE_VITIS
  return std::make_unique<VitisDetector>(model, threshold);
#else
  (void)model;
  (void)threshold;
  throw std::runtime_error(
      "built without Vitis AI (FACESHIELD_WITH_VITIS=OFF); rebuild on the "
      "KV260 or use --detector full-frame");
#endif
}

std::unique_ptr<FaceDetector> make_full_frame_detector() {
  return std::make_unique<FullFrameDetector>();
}

}  // namespace faceshield
