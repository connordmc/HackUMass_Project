// Unit tests for censoring and pipeline strings. No framework needed:
// build with CMake and run `ctest` or ./test_censor.

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "censor.hpp"
#include "pipeline.hpp"
#include "pl_scrubber.hpp"
#include "pl_scrubber_regs.hpp"

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

// True if every luma sample of `r` is 16 and every chroma sample is 128.
static bool is_filled(const Nv12View& v, const cv::Rect& r) {
  const cv::Rect c(r.x / 2, r.y / 2, r.width / 2, r.height / 2);
  std::vector<cv::Mat> ch;
  cv::split(v.uv_plane()(c), ch);
  return cv::countNonZero(v.y_plane()(r) != 16) == 0 &&
         cv::countNonZero(ch[0] != 128) == 0 &&
         cv::countNonZero(ch[1] != 128) == 0;
}

static bool is_all_black(const Nv12View& v) {
  return is_filled(v, cv::Rect(0, 0, v.width, v.height));
}

static void test_censor_inside_only() {
  TestImage img(320, 240);
  const TestImage original = img;
  Nv12View v = img.view();
  const Box face{100, 60, 80, 100, 0.9f};
  CHECK(censor_nv12(v, {face}, 0.0f) == 1);

  cv::Rect r;
  CHECK(expand_and_clip(face, 0.0f, 320, 240, r));
  CHECK(r == cv::Rect(100, 60, 80, 100));
  // Inside: every sample replaced by the constant, nothing of the face left.
  CHECK(is_filled(v, r));
  // Outside the box, including row padding, nothing changed.
  for (int row = 0; row < 240; ++row)
    for (int col = 0; col < img.stride; ++col) {
      const bool inside = r.contains(cv::Point(col, row));
      if (!inside && img.y[row * img.stride + col] !=
                         original.y[row * img.stride + col]) {
        CHECK(!"luma outside the box changed");
        return;
      }
    }
  const cv::Rect chroma(r.x / 2, r.y / 2, r.width / 2, r.height / 2);
  for (int row = 0; row < 120; ++row)
    for (int col = 0; col < img.stride; ++col) {
      const bool inside = chroma.contains(cv::Point(col / 2, row)) && col < 320;
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
  CHECK(censor_nv12(v, {{nan, 1, 2, 3}, {0, 0, -5, 5}, {500, 500, 5, 5}}, 0.2f) == 0);
  CHECK(img.y == original.y && img.uv == original.uv);
}

static void test_tiny_and_full_boxes() {
  TestImage img(64, 48);
  Nv12View v = img.view();
  CHECK(censor_nv12(v, {{10, 10, 1, 1}, {0, 0, 64, 48}}, 0.0f) == 2);
  CHECK(is_all_black(v));
}

static void test_scrub_fails_closed() {
  // No boxes: the whole frame is black.
  TestImage a(64, 48);
  Nv12View va = a.view();
  CHECK(!scrub_nv12(va, {}, 0.2f));
  CHECK(is_all_black(va));
  // Only invalid boxes: same as no face.
  TestImage b(64, 48);
  Nv12View vb = b.view();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK(!scrub_nv12(vb, {{nan, 0, 4, 4}, {200, 0, 4, 4}}, 0.2f));
  CHECK(is_all_black(vb));
  // A valid box: only that box is filled, the rest survives.
  TestImage c(64, 48);
  Nv12View vc = c.view();
  CHECK(scrub_nv12(vc, {{8, 8, 10, 10, 1}}, 0.0f));
  CHECK(is_filled(vc, cv::Rect(8, 8, 10, 10)));
  CHECK(!is_all_black(vc));
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
  try { censor_nv12(v, {}, 0.1f); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
}

static void test_test_mode_no_blank() {
  // No face: the frame is left as it is (test mode only).
  TestImage a(64, 48);
  const TestImage original = a;
  Nv12View va = a.view();
  CHECK(!scrub_nv12(va, {}, 0.2f, /*blank_when_empty=*/false));
  CHECK(a.y == original.y && a.uv == original.uv);
  // A detected face is still filled in test mode.
  TestImage b(64, 48);
  Nv12View vb = b.view();
  CHECK(scrub_nv12(vb, {{8, 8, 10, 10, 1}}, 0.0f, false));
  CHECK(is_filled(vb, cv::Rect(8, 8, 10, 10)));
  // The banner covers the top of the frame.
  TestImage c(320, 240);
  Nv12View vc = c.view();
  draw_test_banner(vc);
  const cv::Mat bar = vc.y_plane()(cv::Rect(0, 0, 320, 20));
  CHECK(cv::countNonZero(bar == 16) > 0 && cv::countNonZero(bar > 200) > 0);
}

static void test_box_hold() {
  using ms = std::chrono::milliseconds;
  const auto t0 = BoxHold::Clock::now();
  BoxHold hold(ms(250), 0.3f);
  const Box a{100, 100, 50, 50, 0.9f}, a_moved{105, 102, 50, 50, 0.9f};
  const Box b{300, 100, 40, 40, 0.8f};
  CHECK(hold.update({a, b}, t0).size() == 2);
  // b missed, still within max age: both faces stay covered.
  auto out = hold.update({a_moved}, t0 + ms(100));
  CHECK(out.size() == 2);
  CHECK(out[0].x == a_moved.x);  // a follows the new detection
  CHECK(hold.update({a_moved}, t0 + ms(250)).size() == 2);  // b exactly at max age
  CHECK(hold.update({a_moved}, t0 + ms(251)).size() == 1);  // b now stale
  // a was last seen at 251 ms: kept until 501 ms, gone after.
  CHECK(hold.update({}, t0 + ms(501)).size() == 1);
  CHECK(hold.update({}, t0 + ms(502)).empty());
  CHECK(std::abs(iou(a, a) - 1.f) < 1e-6f && iou(a, b) == 0.f);
}

static void test_box_hold_time_not_frames() {
  // A slow frame rate must not stretch trust in a stale box.
  using ms = std::chrono::milliseconds;
  const auto t0 = BoxHold::Clock::now();
  BoxHold hold(ms(250), 0.3f);
  hold.update({{0, 0, 10, 10, 1}}, t0);
  CHECK(hold.update({}, t0 + ms(400)).empty());  // one slow frame later
}

static void test_box_hold_future_is_stale() {
  using ms = std::chrono::milliseconds;
  const auto t0 = BoxHold::Clock::now();
  BoxHold hold(ms(250), 0.3f);
  hold.update({{0, 0, 10, 10, 1}}, t0 + ms(50));
  CHECK(hold.update({}, t0).empty());  // stamped after "now": not trusted
  bool threw = false;
  try { BoxHold bad(ms(0), 0.3f); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
}

// Fake AXI-Lite bank that plays the hardware's part of the commit handshake.
struct FakeRegs : RegisterBank {
  std::map<uint32_t, uint32_t>* mem;
  bool auto_latch = true;
  explicit FakeRegs(std::map<uint32_t, uint32_t>* m) : mem(m) {}
  uint32_t read(uint32_t off) override { return (*mem)[off]; }
  void write(uint32_t off, uint32_t v) override {
    (*mem)[off] = v;
    if (off == pl_regs::kCommitSeq && auto_latch) (*mem)[pl_regs::kLatchedSeq] = v;
  }
};

static void test_pl_scrubber_driver() {
  std::map<uint32_t, uint32_t> mem;
  auto regs = std::make_unique<FakeRegs>(&mem);
  FakeRegs* fake = regs.get();
  PlScrubber pl(std::move(regs), 1920, 1080, 8);
  CHECK(mem[pl_regs::kWidth] == 1920 && mem[pl_regs::kHeight] == 1080);
  CHECK(mem[pl_regs::kFillY] == 16 && mem[pl_regs::kFillC] == 128);
  CHECK(mem[pl_regs::kMaxAgeFrames] == 8);

  CHECK(pl.commit({cv::Rect(100, 50, 40, 60)}));
  CHECK(mem[pl_regs::kNumBoxes] == 1);
  CHECK(mem[pl_regs::kBoxX[0]] == (100u | (140u << 16)));
  CHECK(mem[pl_regs::kBoxY[0]] == (50u | (110u << 16)));
  CHECK(mem[pl_regs::kCommitSeq] == 1);

  // No faces: num_boxes = 0, which the hardware blanks.
  CHECK(pl.commit({}));
  CHECK(mem[pl_regs::kNumBoxes] == 0 && mem[pl_regs::kCommitSeq] == 2);

  // Hardware hasn't latched the last commit: the driver must not touch boxes.
  fake->auto_latch = false;
  CHECK(pl.commit({cv::Rect(0, 0, 2, 2)}));
  mem[pl_regs::kBoxX[0]] = 0xdeadbeef;
  CHECK(!pl.commit({cv::Rect(10, 10, 2, 2)}));
  CHECK(mem[pl_regs::kBoxX[0]] == 0xdeadbeef && mem[pl_regs::kCommitSeq] == 3);
  fake->auto_latch = true;
  mem[pl_regs::kLatchedSeq] = 3;

  // Ten faces: the last three merge into one box that covers all of them.
  std::vector<cv::Rect> many;
  for (int i = 0; i < 10; ++i) many.emplace_back(i * 100, i * 50, 20, 20);
  CHECK(pl.commit(many));
  CHECK(mem[pl_regs::kNumBoxes] == 8);
  const uint32_t mx = mem[pl_regs::kBoxX[7]], my = mem[pl_regs::kBoxY[7]];
  CHECK((mx & 0xFFFF) == 700 && (mx >> 16) == 920);
  CHECK((my & 0xFFFF) == 350 && (my >> 16) == 470);

  // Out-of-bounds region: rejected before any register is written.
  const uint32_t seq = mem[pl_regs::kCommitSeq];
  bool threw = false;
  try { pl.commit({cv::Rect(1900, 0, 40, 10)}); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw && mem[pl_regs::kCommitSeq] == seq);
}

static void test_pipelines() {
  StreamConfig c;
  const std::string cap = capture_pipeline(c);
  CHECK(cap.find("mediasrcbin media-device=/dev/media0") != std::string::npos);
  CHECK(cap.find("format=NV12,width=1920,height=1080") != std::string::npos);
  CHECK(cap.find("max-buffers=1 drop=true") != std::string::npos);
  const std::string out = output_pipeline(c, 1920, 1080);
  CHECK(out.find("omxh264enc") != std::string::npos);
  // A viewer joining mid-stream needs an IDR (with SPS/PPS) every second.
  CHECK(out.find("periodicity-idr=30") != std::string::npos);
  CHECK(out.find("config-interval=-1") != std::string::npos);
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
  test_scrub_fails_closed();
  test_test_mode_no_blank();
  test_fill_black();
  test_small_bgr();
  test_invalid_view_rejected();
  test_box_hold();
  test_box_hold_time_not_frames();
  test_box_hold_future_is_stale();
  test_pl_scrubber_driver();
  test_pipelines();
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::puts("all censor tests passed");
  return EXIT_SUCCESS;
}
