#include "censor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

namespace faceshield {

cv::Mat Nv12View::y_plane() const {
  return cv::Mat(height, width, CV_8UC1, y, static_cast<size_t>(y_stride));
}

cv::Mat Nv12View::uv_plane() const {
  return cv::Mat(height / 2, width / 2, CV_8UC2, uv,
                 static_cast<size_t>(uv_stride));
}

static void check_view(const Nv12View& image) {
  if (!image.y || !image.uv || image.width <= 0 || image.height <= 0 ||
      image.width % 2 || image.height % 2 || image.y_stride < image.width ||
      image.uv_stride < image.width) {
    throw std::invalid_argument("invalid NV12 image");
  }
}

bool expand_and_clip(const Box& box, float margin, int image_width,
                     int image_height, cv::Rect& out) {
  const double x = box.x, y = box.y, w = box.width, h = box.height;
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) ||
      !std::isfinite(h) || !std::isfinite(margin) || margin < 0) {
    return false;
  }
  if (w <= 0 || h <= 0) return false;
  if (x >= image_width || y >= image_height || x + w <= 0 || y + h <= 0) {
    return false;
  }
  // Clip in floating point before converting, so huge boxes stay safe.
  const double left = std::max(0.0, x - w * margin);
  const double top = std::max(0.0, y - h * margin);
  const double right = std::min<double>(image_width, x + w + w * margin);
  const double bottom = std::min<double>(image_height, y + h + h * margin);
  // Round outward to even coordinates (image dimensions are even).
  int l = static_cast<int>(std::floor(left)) & ~1;
  int t = static_cast<int>(std::floor(top)) & ~1;
  int r = static_cast<int>(std::ceil(right));
  int b = static_cast<int>(std::ceil(bottom));
  r = std::min(image_width, r + (r & 1));
  b = std::min(image_height, b + (b & 1));
  if (r <= l || b <= t) return false;
  out = cv::Rect(l, t, r - l, b - t);
  return true;
}

static void obscure_plane(cv::Mat roi, int blocks) {
  cv::Mat small;
  cv::resize(roi, small,
             cv::Size(std::min(blocks, roi.cols), std::min(blocks, roi.rows)),
             0, 0, cv::INTER_AREA);
  cv::GaussianBlur(small, small, cv::Size(5, 5), 2.0);
  // roi already has the right size and type, so resize writes in place.
  cv::resize(small, roi, roi.size(), 0, 0, cv::INTER_LINEAR);
}

void obscure_region(Nv12View& image, const cv::Rect& region, int blocks) {
  check_view(image);
  if (blocks < 1) throw std::invalid_argument("blocks must be >= 1");
  const cv::Rect bounds(0, 0, image.width, image.height);
  if ((region & bounds) != region || region.empty() || region.x % 2 ||
      region.y % 2 || region.width % 2 || region.height % 2) {
    throw std::invalid_argument("region must be even-aligned and in bounds");
  }
  obscure_plane(image.y_plane()(region), blocks);
  const cv::Rect chroma(region.x / 2, region.y / 2, region.width / 2,
                        region.height / 2);
  obscure_plane(image.uv_plane()(chroma), std::max(1, blocks / 2));
}

void censor_nv12(Nv12View& image, const std::vector<Box>& boxes, float margin,
                 int blocks) {
  check_view(image);
  for (const Box& box : boxes) {
    cv::Rect region;
    if (expand_and_clip(box, margin, image.width, image.height, region)) {
      obscure_region(image, region, blocks);
    }
  }
}

void fill_black(Nv12View& image) {
  check_view(image);
  image.y_plane().setTo(cv::Scalar(16));          // video-range black
  image.uv_plane().setTo(cv::Scalar(128, 128));   // no colour
}

cv::Mat small_bgr_from_nv12(const Nv12View& image, int out_width,
                            int out_height) {
  check_view(image);
  if (out_width <= 0 || out_height <= 0 || out_width % 2 || out_height % 2) {
    throw std::invalid_argument("detector size must be positive and even");
  }
  cv::Mat nv12(out_height * 3 / 2, out_width, CV_8UC1);
  cv::Mat y_small = nv12.rowRange(0, out_height);
  cv::resize(image.y_plane(), y_small, y_small.size(), 0, 0, cv::INTER_AREA);
  cv::Mat uv_small(out_height / 2, out_width / 2, CV_8UC2,
                   nv12.ptr(out_height), nv12.step);
  cv::resize(image.uv_plane(), uv_small, uv_small.size(), 0, 0,
             cv::INTER_AREA);
  cv::Mat bgr;
  cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
  return bgr;
}

float iou(const Box& a, const Box& b) {
  const float l = std::max(a.x, b.x), t = std::max(a.y, b.y);
  const float r = std::min(a.x + a.width, b.x + b.width);
  const float btm = std::min(a.y + a.height, b.y + b.height);
  const float inter = std::max(0.f, r - l) * std::max(0.f, btm - t);
  const float uni = a.width * a.height + b.width * b.height - inter;
  return uni > 0 ? inter / uni : 0.f;
}

BoxHold::BoxHold(int hold_frames, float match_iou)
    : hold_frames_(std::max(0, hold_frames)), match_iou_(match_iou) {}

std::vector<Box> BoxHold::update(const std::vector<Box>& detections) {
  std::vector<bool> used(detections.size(), false);
  for (Track& track : tracks_) {
    int best = -1;
    float best_iou = match_iou_;
    for (size_t i = 0; i < detections.size(); ++i) {
      if (used[i]) continue;
      const float overlap = iou(track.box, detections[i]);
      if (overlap >= best_iou) {
        best_iou = overlap;
        best = static_cast<int>(i);
      }
    }
    if (best >= 0) {
      used[best] = true;
      track.box = detections[best];
      track.missed = 0;
    } else {
      ++track.missed;
    }
  }
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                               [this](const Track& t) {
                                 return t.missed > hold_frames_;
                               }),
                tracks_.end());
  for (size_t i = 0; i < detections.size(); ++i) {
    if (!used[i]) tracks_.push_back({detections[i], 0});
  }
  std::vector<Box> out;
  out.reserve(tracks_.size());
  for (const Track& track : tracks_) out.push_back(track.box);
  return out;
}

}  // namespace faceshield
