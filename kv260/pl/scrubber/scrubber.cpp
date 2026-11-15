// Vitis HLS top function for the FaceShield PL scrubber. Free running
// (ap_ctrl_none): one AXI4-Stream video beat in, one out, every clock.
// All logic is in scrubber_core.hpp; this file only maps it onto the
// AXI4-Stream and AXI-Lite interfaces.
//
// Build: vitis_hls -f run_hls.tcl   (needs Vitis HLS 2022.1, Linux x86)

#include <ap_axi_sdata.h>
#include <ap_int.h>
#include <hls_stream.h>

#include "scrubber_core.hpp"

#ifndef FACESHIELD_PPC
#define FACESHIELD_PPC 2  // pixels per clock: confirm against the smartcam design
#endif

constexpr int PPC = FACESHIELD_PPC;
constexpr int DATA_BITS = PPC * faceshield_pl::kComponents * 8;
using pixel_beat = ap_axiu<DATA_BITS, 1, 1, 1>;

// Box b is packed as two registers: box<b>_x = x0 | x1 << 16 and
// box<b>_y = y0 | y1 << 16 (half-open pixel ranges).
void faceshield_scrubber(hls::stream<pixel_beat>& s_axis_video,
                         hls::stream<pixel_beat>& m_axis_video,
                         ap_uint<16> width, ap_uint<16> height,
                         ap_uint<8> fill_y, ap_uint<8> fill_c,
                         ap_uint<16> max_age_frames, ap_uint<8> num_boxes,
                         ap_uint<32> box0_x,
                         ap_uint<32> box0_y,
                         ap_uint<32> box1_x,
                         ap_uint<32> box1_y,
                         ap_uint<32> box2_x,
                         ap_uint<32> box2_y,
                         ap_uint<32> box3_x,
                         ap_uint<32> box3_y,
                         ap_uint<32> box4_x,
                         ap_uint<32> box4_y,
                         ap_uint<32> box5_x,
                         ap_uint<32> box5_y,
                         ap_uint<32> box6_x,
                         ap_uint<32> box6_y,
                         ap_uint<32> box7_x,
                         ap_uint<32> box7_y,
                         ap_uint<32> commit_seq,
                         ap_uint<32>* frames, ap_uint<32>* blanked_frames,
                         ap_uint<32>* frames_since_commit,
                         ap_uint<32>* latched_seq) {
#pragma HLS INTERFACE mode=ap_ctrl_none port=return
#pragma HLS INTERFACE mode=axis register_mode=both port=s_axis_video
#pragma HLS INTERFACE mode=axis register_mode=both port=m_axis_video
#pragma HLS INTERFACE mode=s_axilite port=width bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=height bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=fill_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=fill_c bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=max_age_frames bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=num_boxes bundle=ctrl
// Boxes are scalar registers, not an array: an AXI-Lite array becomes a RAM,
// and the start-of-frame latch must read all of them in one cycle.
#pragma HLS INTERFACE mode=s_axilite port=box0_x bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box0_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box1_x bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box1_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box2_x bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box2_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box3_x bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box3_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box4_x bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box4_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box5_x bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box5_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box6_x bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box6_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box7_x bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=box7_y bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=commit_seq bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=frames bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=blanked_frames bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=frames_since_commit bundle=ctrl
#pragma HLS INTERFACE mode=s_axilite port=latched_seq bundle=ctrl
#pragma HLS PIPELINE II=1 style=flp

  static faceshield_pl::Scrubber<PPC> scrubber;
  static faceshield_pl::Status status;

  faceshield_pl::Registers regs;
  regs.width = width;
  regs.height = height;
  regs.fill_y = fill_y;
  regs.fill_c = fill_c;
  regs.max_age_frames = max_age_frames;
  regs.num_boxes = num_boxes;
  regs.commit_seq = commit_seq;
  const ap_uint<32> box_x[faceshield_pl::kMaxBoxes] = {
      box0_x, box1_x, box2_x, box3_x, box4_x, box5_x, box6_x, box7_x};
  const ap_uint<32> box_y[faceshield_pl::kMaxBoxes] = {
      box0_y, box1_y, box2_y, box3_y, box4_y, box5_y, box6_y, box7_y};
  for (int b = 0; b < faceshield_pl::kMaxBoxes; ++b) {
#pragma HLS UNROLL
    regs.x0[b] = box_x[b].range(15, 0);
    regs.x1[b] = box_x[b].range(31, 16);
    regs.y0[b] = box_y[b].range(15, 0);
    regs.y1[b] = box_y[b].range(31, 16);
  }

  pixel_beat in = s_axis_video.read();
  faceshield_pl::Beat<PPC> beat;
  for (int i = 0; i < PPC * faceshield_pl::kComponents; ++i) {
#pragma HLS UNROLL
    beat.data[i] = in.data.range(i * 8 + 7, i * 8);
  }
  beat.user = in.user;
  beat.last = in.last;

  scrubber.step(regs, beat, status);

  pixel_beat out = in;  // keep/strb/id/dest pass through unchanged
  for (int i = 0; i < PPC * faceshield_pl::kComponents; ++i) {
#pragma HLS UNROLL
    out.data.range(i * 8 + 7, i * 8) = beat.data[i];
  }
  m_axis_video.write(out);

  *frames = status.frames;
  *blanked_frames = status.blanked_frames;
  *frames_since_commit = status.frames_since_commit;
  *latched_seq = status.latched_seq;
}
