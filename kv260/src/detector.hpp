// Face detectors. Each returns boxes in full-frame pixel coordinates.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "censor.hpp"

namespace faceshield {

class FaceDetector {
 public:
  virtual ~FaceDetector() = default;
  // Size of the BGR image detect() expects.
  virtual cv::Size input_size() const = 0;
  // `bgr` has input_size(); boxes are returned scaled to full_size.
  virtual std::vector<Box> detect(const cv::Mat& bgr, cv::Size full_size) = 0;
};

// AMD Vitis AI Library densebox face detector, running on the DPU.
// Only available when built with FACESHIELD_WITH_VITIS=ON.
std::unique_ptr<FaceDetector> make_vitis_detector(const std::string& model,
                                                  float threshold);

// Bring-up detector that reports the whole frame as one face, so everything
// is obscured. Lets the camera and streaming path be tested without the DPU
// while never sending a clear image.
std::unique_ptr<FaceDetector> make_full_frame_detector();

}  // namespace faceshield
