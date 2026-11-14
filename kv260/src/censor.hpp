// NV12 face censoring. Pure OpenCV: no GStreamer or Vitis AI, so it is unit
// tested on any machine.
#pragma once

#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>

namespace faceshield {

// A face box in full-frame pixel coordinates.
struct Box {
  float x = 0, y = 0, width = 0, height = 0;
  float score = 0;
};

// A writable NV12 image: a full-resolution Y plane followed (elsewhere in
// memory) by a half-resolution interleaved UV plane. Width and height must be
// even. Strides are in bytes and may exceed the width (padded rows).
struct Nv12View {
  uint8_t* y = nullptr;
  int y_stride = 0;
  uint8_t* uv = nullptr;
  int uv_stride = 0;
  int width = 0;
  int height = 0;

  cv::Mat y_plane() const;   // CV_8UC1, height x width
  cv::Mat uv_plane() const;  // CV_8UC2, height/2 x width/2
};

// Adds `margin` (fraction of the box size) on every side, clips to the image
// and rounds outward to even coordinates so the box maps exactly onto the
// half-resolution chroma plane. Returns false for malformed, non-finite,
// empty or fully off-image boxes.
bool expand_and_clip(const Box& box, float margin, int image_width,
                     int image_height, cv::Rect& out);

// Obscures one even-aligned region in both planes: reduce it to at most
// `blocks` x `blocks` samples, smooth, and scale back up.
void obscure_region(Nv12View& image, const cv::Rect& region, int blocks);

// Obscures every valid box. Invalid boxes are ignored.
void censor_nv12(Nv12View& image, const std::vector<Box>& boxes, float margin,
                 int blocks);

// Overwrites the whole image with black (used when detection fails).
void fill_black(Nv12View& image);

// Cheap BGR thumbnail for the detector: scales the NV12 planes first, then
// converts colour on the small image only.
cv::Mat small_bgr_from_nv12(const Nv12View& image, int out_width,
                            int out_height);

float iou(const Box& a, const Box& b);

// Keeps each face covered for `hold_frames` frames after the detector stops
// reporting it, so a single missed detection does not expose a face.
class BoxHold {
 public:
  BoxHold(int hold_frames, float match_iou);
  std::vector<Box> update(const std::vector<Box>& detections);
  void clear() { tracks_.clear(); }

 private:
  struct Track {
    Box box;
    int missed = 0;
  };
  int hold_frames_;
  float match_iou_;
  std::vector<Track> tracks_;
};

}  // namespace faceshield
