// Unit tests for censoring and pipeline strings. No framework needed:
// build with CMake and run `ctest` or ./test_censor.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "censor.hpp"
#include "pipeline.hpp"

using namespace faceshield;

static int g_failures = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,    \
                   __LINE__, #cond);                                 \
      ++g_failures;                                                  \
    }                                                                \
  } while (0)

// NV12 image with random content and padded rows (stride > width), so tests
// also prove the padding is never touched.
struct TestImage {
  int w, h, stride;
  std::vector<uint8_t> y, uv;
  TestImage(int w_, int h_, int pad = 32) : w(w_), h(h_), stride(w_ + pad) {
    y.resize(static_cast<size_t>(stride) * h);
    uv.resize(static_cast<size_t>(stride) * h / 2);
    cv::RNG rng(1234);
    cv::Mat ym(h, stride, CV_8UC1, y.data()), uvm(h / 2, stride, CV_8UC1, uv.data());
    rng.fill(ym, cv::RNG::UNIFORM, 0, 256);
    rng.fill(uvm, cv::RNG::UNIFORM, 0, 256);
  }
  Nv12View view() { return {y.data(), stride, uv.data(), stride, w, h}; }
};

static double stddev(const cv::Mat& m) {
  cv::Scalar mean, sd;
  cv::meanStdDev(m, mean, sd);
  return sd[0];
}

static void test_censor_inside_only() {
  TestImage img(320, 240);
  const TestImage original = img;
  Nv12View v = img.view();
  const Box face{100, 60, 80, 100, 0.9f};
  censor_nv12(v, {face}, 0.0f, 8);

  cv::Rect r;
  CHECK(expand_and_clip(face, 0.0f, 320, 240, r));
  CHECK(r == cv::Rect(100, 60, 80, 100));
  // Inside: detail is gone (random noise has stddev ~74).
  CHECK(stddev(v.y_plane()(r)) < 15);
  // Outside the box, including row padding, nothing changed.
  for (int row = 0; row < 240; ++row)
    for (int col = 0; col < img.stride; ++col) {
      const bool inside = r.contains({col, row});
      if (!inside && img.y[row * img.stride + col] !=
                         original.y[row * img.stride + col]) {
        CHECK(!"luma outside the box changed");
        return;
      }
    }
  const cv::Rect chroma(r.x / 2, r.y / 2, r.width / 2, r.height / 2);
  for (int row = 0; row < 120; ++row)
    for (int col = 0; col < img.stride; ++col) {
      const bool inside = chroma.contains({col / 2, row}) && col < 320;
      if (!inside && img.uv[row * img.stride + col] !=
                         original.uv[row * img.stride + col]) {
        CHECK(!"chroma outside the box changed");
        return;
      }
    }
}

static void test_expand_and_clip() {
  cv::Rect r;
  // Margin, odd coordinates and clipping at the right/bottom edge.
  CHECK(expand_and_clip({301, 221, 30, 30}, 0.5f, 320, 240, r));
  CHECK(r.x % 2 == 0 && r.y % 2 == 0 && r.width % 2 == 0 && r.height % 2 == 0);
  CHECK(r.x <= 286 && r.y <= 206 && r.br().x == 320 && r.br().y == 240);
  // Left/top edge.
  CHECK(expand_and_clip({-50, -50, 70, 70}, 0.2f, 320, 240, r));
  CHECK(r.x == 0 && r.y == 0 && r.br().x >= 34);
  // Huge box covers the image exactly.
  CHECK(expand_and_clip({-1e30f, -1e30f, 3e30f, 3e30f}, 0.1f, 320, 240, r));
  CHECK(r == cv::Rect(0, 0, 320, 240));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  CHECK(!expand_and_clip({nan, 0, 10, 10}, 0.1f, 320, 240, r));
  CHECK(!expand_and_clip({0, 0, inf, 10}, 0.1f, 320, 240, r));
  CHECK(!expand_and_clip({0, 0, 0, 10}, 0.1f, 320, 240, r));
  CHECK(!expand_and_clip({0, 0, 10, -1}, 0.1f, 320, 240, r));
  CHECK(!expand_and_clip({320, 0, 10, 10}, 0.1f, 320, 240, r));
  CHECK(!expand_and_clip({-10, 0, 10, 10}, 0.1f, 320, 240, r));
  CHECK(!expand_and_clip({0, 0, 10, 10}, -0.1f, 320, 240, r));
}

static void test_invalid_boxes_ignored() {
  TestImage img(64, 48);
  const TestImage original = img;
  Nv12View v = img.view();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  censor_nv12(v, {{nan, 1, 2, 3}, {0, 0, -5, 5}, {500, 500, 5, 5}}, 0.2f, 8);
  CHECK(img.y == original.y && img.uv == original.uv);
}

static void test_tiny_and_full_boxes() {
  TestImage img(64, 48);
  Nv12View v = img.view();
  censor_nv12(v, {{10, 10, 1, 1}, {0, 0, 64, 48}}, 0.0f, 8);  // no throw
  CHECK(stddev(v.y_plane()) < 20);
}

static void test_fill_black() {
  TestImage img(64, 48);
  Nv12View v = img.view();
  fill_black(v);
  CHECK(cv::countNonZero(v.y_plane() != 16) == 0);
  std::vector<cv::Mat> ch;
  cv::split(v.uv_plane(), ch);
  CHECK(cv::countNonZero(ch[0] != 128) == 0 && cv::countNonZero(ch[1] != 128) == 0);
}

static void test_small_bgr() {
  TestImage img(1920, 1080, 128);
  Nv12View v = img.view();
  v.y_plane().setTo(cv::Scalar(126));
  v.uv_plane().setTo(cv::Scalar(128, 128));
  const cv::Mat bgr = small_bgr_from_nv12(v, 640, 360);
  CHECK(bgr.cols == 640 && bgr.rows == 360 && bgr.type() == CV_8UC3);
  const cv::Scalar mean = cv::mean(bgr);
  CHECK(std::abs(mean[0] - mean[1]) < 3 && std::abs(mean[1] - mean[2]) < 3);
  CHECK(mean[0] > 100 && mean[0] < 160);
}

static void test_invalid_view_rejected() {
  TestImage img(64, 48);
  Nv12View v = img.view();
  v.width = 63;
  bool threw = false;
  try { censor_nv12(v, {}, 0.1f, 8); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
}

static void test_box_hold() {
  BoxHold hold(2, 0.3f);
  const Box a{100, 100, 50, 50, 0.9f}, a_moved{105, 102, 50, 50, 0.9f};
  const Box b{300, 100, 40, 40, 0.8f};
  CHECK(hold.update({a, b}).size() == 2);
  auto out = hold.update({a_moved});  // b missed once: still covered
  CHECK(out.size() == 2);
  CHECK(out[0].x == a_moved.x);       // a follows the new detection
  CHECK(hold.update({a_moved}).size() == 2);  // b missed twice
  CHECK(hold.update({a_moved}).size() == 1);  // b dropped after hold
  CHECK(hold.update({}).size() == 1);
  CHECK(hold.update({}).size() == 1);
  CHECK(hold.update({}).empty());
  CHECK(std::abs(iou(a, a) - 1.f) < 1e-6f && iou(a, b) == 0.f);
}

static void test_pipelines() {
  StreamConfig c;
  const std::string cap = capture_pipeline(c);
  CHECK(cap.find("mediasrcbin media-device=/dev/media0") != std::string::npos);
  CHECK(cap.find("format=NV12,width=1920,height=1080") != std::string::npos);
  CHECK(cap.find("max-buffers=1 drop=true") != std::string::npos);
  const std::string out = output_pipeline(c, 1920, 1080);
  CHECK(out.find("omxh264enc") != std::string::npos);
  CHECK(out.find("tcpserversink host=0.0.0.0 port=5000") != std::string::npos);
  c.transport = "udp";
  bool threw = false;
  try { output_pipeline(c, 1920, 1080); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  c.host = "192.168.1.20";
  CHECK(output_pipeline(c, 1920, 1080).find("udpsink host=192.168.1.20") != std::string::npos);
  c.source = "videotestsrc";
  CHECK(capture_pipeline(c).rfind("videotestsrc ! videoconvert", 0) == 0);
}

int main() {
  test_censor_inside_only();
  test_expand_and_clip();
  test_invalid_boxes_ignored();
  test_tiny_and_full_boxes();
  test_fill_black();
  test_small_bgr();
  test_invalid_view_rejected();
  test_box_hold();
  test_pipelines();
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::puts("all censor tests passed");
  return EXIT_SUCCESS;
}
