// AXI-Lite register offsets of the faceshield_scrubber HLS IP.
//
// PREDICTED, not generated: Vitis HLS assigns these at synthesis. After
// running pl/scrubber/run_hls.tcl, regenerate this file from the real
// driver header with:
//   python3 pl/scrubber/gen_regs.py <HLS export>/drivers/*/src/xfaceshield_scrubber_hw.h
//     > src/pl_scrubber_regs.hpp
// where <HLS export> is pl/scrubber/faceshield_scrubber_prj/solution1/impl/ip
#pragma once

#include <cstdint>

namespace faceshield::pl_regs {

constexpr bool kGenerated = false;

constexpr uint32_t kWidth = 0x10;
constexpr uint32_t kHeight = 0x18;
constexpr uint32_t kFillY = 0x20;
constexpr uint32_t kFillC = 0x28;
constexpr uint32_t kMaxAgeFrames = 0x30;
constexpr uint32_t kNumBoxes = 0x38;
// box<b>_x at kBoxX[b], box<b>_y at kBoxY[b]
constexpr uint32_t kBoxX[8] = {0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0};
constexpr uint32_t kBoxY[8] = {0x48, 0x58, 0x68, 0x78, 0x88, 0x98, 0xa8, 0xb8};
constexpr uint32_t kCommitSeq = 0xc0;
constexpr uint32_t kFrames = 0xc8;
constexpr uint32_t kBlankedFrames = 0xd8;
constexpr uint32_t kFramesSinceCommit = 0xe8;
constexpr uint32_t kLatchedSeq = 0xf8;
constexpr uint32_t kSpan = 0x100;  // bytes to map

}  // namespace faceshield::pl_regs
