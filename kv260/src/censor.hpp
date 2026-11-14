// NV12 face censoring, fail-closed. Pure OpenCV: no GStreamer or Vitis AI,
// so it is unit tested on any machine.
//
// Policy (same as the Python prototype, faceshield/scrubber.py):
//  - every current face box is overwritten with a constant fill (video black);
//    the original pixels are destroyed, not blurred;
//  - a box stays current for max_age after the detector last saw it;
//  - if no box is current, the whole frame is blacked out.
#pragma once

#include <chrono>
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

// Overwrites one even-aligned, in-bounds region in both planes with black
// (Y = 16, UV = 128). The original pixels are gone afterwards.
void fill_region(Nv12View& image, const cv::Rect& region);

// Fills every valid box. Invalid boxes are ignored. Returns how many regions
// were filled.
int censor_nv12(Nv12View& image, const std::vector<Box>& boxes, float margin);

// Fail-closed scrub: fills every valid box and returns true, or, if no box
// maps to a valid region, blacks out the whole frame and returns false.
bool scrub_nv12(Nv12View& image, const std::vector<Box>& boxes, float margin);

// Overwrites the whole image with black.
void fill_black(Nv12View& image);

// Cheap BGR thumbnail for the detector: scales the NV12 planes first, then
// converts colour on the small image only.
cv::Mat small_bgr_from_nv12(const Nv12View& image, int out_width,
                            int out_height);

float iou(const Box& a, const Box& b);

// Keeps each face covered for `max_age` after the detector last reported it,
// so a brief miss does not expose a face. Time-based, so a slow frame rate
// cannot stretch how long a stale box is trusted. Each face is tracked
// separately: if one of two faces is missed, both stay covered.
class BoxHold {
 public:
  using Clock = std::chrono::steady_clock;

  BoxHold(std::chrono::milliseconds max_age, float match_iou);
  // Adds this frame's detections (captured at `captured_at`) and returns every
  // box that is still current at `captured_at`. A track whose last sighting is
  // older than max_age, or in the future, is dropped.
  std::vector<Box> update(const std::vector<Box>& detections,
                          Clock::time_point captured_at);
  void clear() { tracks_.clear(); }

 private:
  struct Track {
    Box box;
    Clock::time_point last_seen;
  };
  std::chrono::milliseconds max_age_;
  float match_iou_;
  std::vector<Track> tracks_;
};

}  // namespace faceshield
