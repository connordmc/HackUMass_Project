// C testbench for the PL scrubber logic. Runs with plain g++ (run_csim.sh)
// and as the Vitis HLS C-simulation testbench (run_hls.tcl).

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "scrubber_core.hpp"

using namespace faceshield_pl;

constexpr int PPC = 2;
constexpr int W = 32, H = 16;

static int g_failures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                  \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

// A frame as the scrubber outputs it: Y and C per pixel.
struct Frame {
  int w, h;
  std::vector<uint8_t> y, c;
  Frame(int w_, int h_) : w(w_), h(h_), y(w_ * h_), c(w_ * h_) {}
  uint8_t Y(int x, int r) const { return y[r * w + x]; }
  uint8_t C(int x, int r) const { return c[r * w + x]; }
};

// Input pixel values are distinctive and never equal to the fill values.
static uint8_t in_y(int x, int r) { return static_cast<uint8_t>(200 + (x + r) % 40); }
static uint8_t in_c(int x, int r) { return static_cast<uint8_t>(30 + (x * 3 + r) % 60); }

// Streams one frame (lines of `line_w` pixels, `lines` lines) through the
// scrubber. `regs_mid` (if given) is applied halfway through the frame to
// check that changes only take effect at the next start of frame.
static Frame run_frame(Scrubber<PPC>& s, Registers& regs, Status& st,
                       int line_w = W, int lines = H, bool with_user = true,
                       const Registers* regs_mid = nullptr) {
  Frame out(line_w, lines);
  for (int r = 0; r < lines; ++r) {
    const Registers& active = (regs_mid && r >= lines / 2) ? *regs_mid : regs;
    for (int x = 0; x < line_w; x += PPC) {
      Beat<PPC> b;
      for (int p = 0; p < PPC; ++p) {
        b.data[p * 2] = in_y(x + p, r);
        b.data[p * 2 + 1] = in_c(x + p, r);
      }
      b.user = with_user && r == 0 && x == 0;
      b.last = x + PPC >= line_w;
      s.step(active, b, st);
      for (int p = 0; p < PPC; ++p) {
        out.y[r * line_w + x + p] = b.data[p * 2];
        out.c[r * line_w + x + p] = b.data[p * 2 + 1];
      }
    }
  }
  return out;
}

static bool filled(const Frame& f, int x, int r) {
  return f.Y(x, r) == 16 && f.C(x, r) == 128;
}
static bool passed(const Frame& f, int x, int r) {
  return f.Y(x, r) == in_y(x, r) && f.C(x, r) == in_c(x, r);
}
static bool all_filled(const Frame& f) {
  for (int r = 0; r < f.h; ++r)
    for (int x = 0; x < f.w; ++x)
      if (!filled(f, x, r)) return false;
  return true;
}

static Registers base_regs() {
  Registers r;
  r.width = W;
  r.height = H;
  r.max_age_frames = 3;
  return r;
}

static void commit(Registers& r, std::vector<std::array<uint16_t, 4>> boxes) {
  r.num_boxes = static_cast<uint8_t>(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    r.x0[i] = boxes[i][0];
    r.y0[i] = boxes[i][1];
    r.x1[i] = boxes[i][2];
    r.y1[i] = boxes[i][3];
  }
  ++r.commit_seq;
}

static void test_reset_blanks_everything() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  // Registers hold boxes but nothing was ever committed (seq unchanged).
  r.num_boxes = 1;
  r.x1[0] = 4;
  r.y1[0] = 4;
  CHECK(all_filled(run_frame(s, r, st)));
  CHECK(st.frames == 1 && st.blanked_frames == 1);
}

static void test_box_filled_rest_passes() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {{4, 2, 10, 8}});
  const Frame f = run_frame(s, r, st);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const bool inside = x >= 4 && x < 10 && y >= 2 && y < 8;
      if (inside) CHECK(filled(f, x, y));
      else CHECK(passed(f, x, y));
    }
  CHECK(st.blanked_frames == 0 && st.latched_seq == r.commit_seq);
}

static void test_multiple_boxes() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {{0, 0, 2, 2}, {30, 14, 32, 16}, {10, 5, 12, 6}});
  const Frame f = run_frame(s, r, st);
  CHECK(filled(f, 0, 0) && filled(f, 1, 1));
  CHECK(filled(f, 31, 15) && filled(f, 30, 14));
  CHECK(filled(f, 10, 5) && filled(f, 11, 5));
  CHECK(passed(f, 2, 0) && passed(f, 12, 5) && passed(f, 10, 6) && passed(f, 20, 8));
}

static void test_odd_box_edges_inside_one_beat() {
  // x0 = 3 and x1 = 5 split two-pixel beats: each pixel is tested separately.
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {{3, 0, 5, 1}});
  const Frame f = run_frame(s, r, st);
  CHECK(passed(f, 2, 0) && filled(f, 3, 0) && filled(f, 4, 0) && passed(f, 5, 0));
}

static void test_goes_stale_after_max_age() {
  Scrubber<PPC> s;
  Registers r = base_regs();  // max_age_frames = 3
  Status st;
  commit(r, {{0, 0, 2, 2}});
  for (int i = 0; i <= 3; ++i) CHECK(passed(run_frame(s, r, st), 20, 8));
  CHECK(st.frames_since_commit == 3);
  CHECK(all_filled(run_frame(s, r, st)));  // 4 frames without a commit
  commit(r, {{0, 0, 2, 2}});
  CHECK(passed(run_frame(s, r, st), 20, 8));  // a new commit revives it
}

static void test_zero_boxes_blank() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {});  // software says "no face": fail closed
  CHECK(all_filled(run_frame(s, r, st)));
}

static void test_malformed_boxes_blank() {
  const std::vector<std::array<uint16_t, 4>> bad[] = {
      {{5, 0, 5, 4}},          // empty width
      {{0, 6, 4, 2}},          // inverted
      {{0, 0, W + 2, 4}},      // past the right edge
      {{0, 0, 4, H + 1}},      // past the bottom
      {{0, 0, 2, 2}, {9, 9, 3, 3}},  // one good, one bad
  };
  for (const auto& boxes : bad) {
    Scrubber<PPC> s;
    Registers r = base_regs();
    Status st;
    commit(r, boxes);
    CHECK(all_filled(run_frame(s, r, st)));
  }
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  r.num_boxes = kMaxBoxes + 1;
  ++r.commit_seq;
  CHECK(all_filled(run_frame(s, r, st)));
}

static void test_boxes_change_only_at_frame_start() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {{0, 0, 4, 16}});  // left strip
  Registers moved = r;
  commit(moved, {{28, 0, 32, 16}});  // right strip, committed mid-frame
  const Frame f = run_frame(s, r, st, W, H, true, &moved);
  CHECK(filled(f, 0, 15) && passed(f, 30, 15));  // old box for the whole frame
  const Frame g = run_frame(s, moved, st);
  CHECK(passed(g, 0, 15) && filled(g, 30, 15));  // new box from the next frame
}

static void test_uncommitted_box_writes_ignored() {
  // Software rewrites boxes without bumping commit_seq: the latched boxes stay.
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {{0, 0, 4, 4}});
  run_frame(s, r, st);
  r.x0[0] = 20;
  r.x1[0] = 24;
  const Frame f = run_frame(s, r, st);
  CHECK(filled(f, 0, 0) && passed(f, 21, 0));
}

static void test_geometry_mismatch_fills() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {{0, 0, 2, 2}});
  // Lines wider and a frame taller than configured: extra pixels are unknown.
  const Frame f = run_frame(s, r, st, W + 4, H + 2);
  CHECK(passed(f, 20, 8));
  CHECK(filled(f, W, 3) && filled(f, W + 3, 3));
  CHECK(filled(f, 20, H) && filled(f, 20, H + 1));
}

static void test_stream_without_start_of_frame_fills() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {{0, 0, 2, 2}});
  CHECK(all_filled(run_frame(s, r, st, W, H, /*with_user=*/false)));
  CHECK(st.frames == 0);
}

static void test_resync_after_short_line() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  commit(r, {{0, 0, 2, 2}});
  run_frame(s, r, st, W - 6, H);  // a broken frame with short lines
  // The next start of frame resets the counters: the box lands correctly.
  const Frame f = run_frame(s, r, st);
  CHECK(filled(f, 0, 0) && filled(f, 1, 1) && passed(f, 2, 0) && passed(f, 0, 2));
}

static void test_custom_fill() {
  Scrubber<PPC> s;
  Registers r = base_regs();
  Status st;
  r.fill_y = 0;
  r.fill_c = 0;
  commit(r, {{0, 0, 2, 2}});
  const Frame f = run_frame(s, r, st);
  CHECK(f.Y(0, 0) == 0 && f.C(0, 0) == 0);
}

int main() {
  test_reset_blanks_everything();
  test_box_filled_rest_passes();
  test_multiple_boxes();
  test_odd_box_edges_inside_one_beat();
  test_goes_stale_after_max_age();
  test_zero_boxes_blank();
  test_malformed_boxes_blank();
  test_boxes_change_only_at_frame_start();
  test_uncommitted_box_writes_ignored();
  test_geometry_mismatch_fills();
  test_stream_without_start_of_frame_fills();
  test_resync_after_short_line();
  test_custom_fill();
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::puts("all scrubber tests passed");
  return EXIT_SUCCESS;
}
