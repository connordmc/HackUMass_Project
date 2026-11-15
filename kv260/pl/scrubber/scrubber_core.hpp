// FaceShield PL scrubber: per-beat logic, shared by the HLS top function
// (scrubber.cpp) and the C testbench (tb_scrubber.cpp). Plain C++ with fixed
// width integers, so it builds with any compiler and with Vitis HLS.
//
// The block sits on the AXI4-Stream video path between the camera receiver
// and the frame-buffer write DMA that Linux reads. Every pixel inside a box,
// and every pixel of a frame without a current commit, is replaced with a
// constant before it reaches memory. Nothing is kept: there is no line buffer,
// no frame buffer and no readback of pixel data.
//
// Stream format (UG934 AXI4-Stream video, YUV 4:2:0 8-bit): each pixel is two
// 8-bit components, byte 0 = Y and byte 1 = C (Cb/Cr alternating, present on
// chroma lines). PPC pixels per beat, so TDATA is PPC*16 bits. TUSER marks the
// first beat of a frame, TLAST the last beat of a line.
// CONFIRM AGAINST THE SMARTCAM DESIGN before building (see ../README.md).
#pragma once

#include <cstdint>

namespace faceshield_pl {

constexpr int kMaxBoxes = 8;
constexpr int kComponents = 2;  // Y, C per pixel

// Register values as software writes them (AXI-Lite). Boxes are half-open
// pixel rectangles [x0, x1) x [y0, y1), already padded by software.
struct Registers {
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t fill_y = 16;    // video-range black
  uint8_t fill_c = 128;   // no colour
  uint16_t max_age_frames = 8;  // ~250 ms at 30 fps
  uint8_t num_boxes = 0;
  uint16_t x0[kMaxBoxes] = {}, y0[kMaxBoxes] = {};
  uint16_t x1[kMaxBoxes] = {}, y1[kMaxBoxes] = {};
  // Commit handshake: software writes num_boxes and all boxes, then changes
  // commit_seq. The block copies the boxes only at a start of frame where
  // commit_seq changed, and reports it in Status::latched_seq. Software must not
  // touch the boxes again until latched_seq == commit_seq, so a frame can never
  // see half of one commit and half of the next.
  uint32_t commit_seq = 0;
};

struct Status {
  uint32_t frames = 0;
  uint32_t blanked_frames = 0;
  uint32_t frames_since_commit = 0;
  uint32_t latched_seq = 0;
};

template <int PPC>
struct Beat {
  uint8_t data[PPC * kComponents];
  bool user;  // start of frame
  bool last;  // end of line
};

template <int PPC>
class Scrubber {
 public:
  // Processes one beat. Fails closed: until a valid, current commit has been
  // latched at a start of frame, every sample is filled.
  void step(const Registers& regs, Beat<PPC>& beat, Status& status) {
#pragma HLS INLINE
    if (beat.user) start_frame(regs, status);

    // Pixels outside the configured geometry (a longer line or frame than
    // expected) are unknown: fill them. Before the first start of frame,
    // blank_ is still true from reset, so a stream without TUSER is filled.
    const bool outside = row_ >= height_;
    for (int p = 0; p < PPC; ++p) {
#pragma HLS UNROLL
      const uint32_t x = col_ + p;
      bool fill = blank_ || outside || x >= width_;
      for (int b = 0; b < kMaxBoxes; ++b) {
#pragma HLS UNROLL
        fill |= b < n_ && x >= x0_[b] && x < x1_[b] && row_ >= y0_[b] &&
                row_ < y1_[b];
      }
      if (fill) {
        beat.data[p * kComponents + 0] = fy_;
        beat.data[p * kComponents + 1] = fc_;
      }
    }

    if (beat.last) {
      col_ = 0;
      if (row_ < 0xFFFF) ++row_;
    } else if (col_ < 0xFFFF) {
      col_ += PPC;
    }
  }

 private:
  void start_frame(const Registers& regs, Status& status) {
#pragma HLS INLINE
    if (regs.commit_seq != last_seq_) {
      // New commit: copy the boxes now, while software is not writing them.
      last_seq_ = regs.commit_seq;
      committed_ = true;
      since_commit_ = 0;
      n_ = regs.num_boxes;
      for (int b = 0; b < kMaxBoxes; ++b) {
#pragma HLS UNROLL
        x0_[b] = regs.x0[b];
        y0_[b] = regs.y0[b];
        x1_[b] = regs.x1[b];
        y1_[b] = regs.y1[b];
      }
    } else if (since_commit_ < 0xFFFF) {
      ++since_commit_;
    }

    // Frame geometry and fill are static configuration, latched every frame.
    width_ = regs.width;
    height_ = regs.height;
    fy_ = regs.fill_y;
    fc_ = regs.fill_c;

    bool bad = width_ == 0 || height_ == 0 || n_ == 0 || n_ > kMaxBoxes;
    for (int b = 0; b < kMaxBoxes; ++b) {
#pragma HLS UNROLL
      // A malformed box means software is confused: trust nothing.
      if (b < n_ && (x0_[b] >= x1_[b] || y0_[b] >= y1_[b] ||
                     x1_[b] > width_ || y1_[b] > height_)) {
        bad = true;
      }
    }
    blank_ = !committed_ || bad || since_commit_ > regs.max_age_frames;

    row_ = 0;
    col_ = 0;
    ++status.frames;
    if (blank_) ++status.blanked_frames;
    status.frames_since_commit = since_commit_;
    status.latched_seq = last_seq_;
  }

  // Reset state: no frame seen, nothing committed, everything filled.
  bool committed_ = false;
  bool blank_ = true;
  uint32_t last_seq_ = 0;
  uint16_t since_commit_ = 0;
  uint16_t row_ = 0, col_ = 0;
  uint16_t width_ = 0, height_ = 0;
  uint8_t fy_ = 16, fc_ = 128, n_ = 0;
  uint16_t x0_[kMaxBoxes] = {}, y0_[kMaxBoxes] = {};
  uint16_t x1_[kMaxBoxes] = {}, y1_[kMaxBoxes] = {};
};

}  // namespace faceshield_pl
