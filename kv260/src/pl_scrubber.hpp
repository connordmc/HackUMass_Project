// Driver for the PL scrubber IP (pl/scrubber/). Software only tells the
// hardware where faces are; the hardware fills them, and blanks any frame
// without a current commit, before the clean frame reaches memory.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace faceshield {

// 32-bit register access, so tests can use a fake instead of /dev/uio.
class RegisterBank {
 public:
  virtual ~RegisterBank() = default;
  virtual uint32_t read(uint32_t offset) = 0;
  virtual void write(uint32_t offset, uint32_t value) = 0;
};

// Maps the scrubber's AXI-Lite registers from a UIO device (e.g. /dev/uio4,
// from the generic-uio node in the faceshield device-tree overlay).
std::unique_ptr<RegisterBank> open_uio_registers(const std::string& device);

struct PlStatus {
  uint32_t frames = 0;
  uint32_t blanked_frames = 0;
  uint32_t frames_since_commit = 0;
};

class PlScrubber {
 public:
  static constexpr int kMaxBoxes = 8;

  PlScrubber(std::unique_ptr<RegisterBank> regs, int width, int height,
             int max_age_frames);

  // Sends this frame's face regions (even-aligned, in bounds, already padded;
  // see expand_and_clip). Empty = no face = the hardware blanks the frame.
  // More than kMaxBoxes regions: the extras are merged into one covering box,
  // which only ever masks more. Returns false, without writing, if the
  // hardware has not latched the previous commit yet; that commit keeps aging
  // in hardware, so skipping is safe.
  bool commit(const std::vector<cv::Rect>& regions);

  PlStatus status();

 private:
  std::unique_ptr<RegisterBank> regs_;
  int width_, height_;
  uint32_t seq_;
};

}  // namespace faceshield
