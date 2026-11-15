#include "pl_scrubber.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pl_scrubber_regs.hpp"

namespace faceshield {

namespace {

class UioRegisters : public RegisterBank {
 public:
  explicit UioRegisters(const std::string& device) {
    fd_ = ::open(device.c_str(), O_RDWR | O_SYNC);
    if (fd_ < 0)
      throw std::runtime_error("cannot open PL scrubber " + device + ": " +
                               std::strerror(errno) +
                               " (is the kv260-faceshield firmware loaded?)");
    void* p = ::mmap(nullptr, pl_regs::kSpan, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
      const int err = errno;
      ::close(fd_);
      throw std::runtime_error("cannot map PL scrubber registers: " +
                               std::string(std::strerror(err)));
    }
    base_ = static_cast<volatile uint32_t*>(p);
  }
  ~UioRegisters() override {
    ::munmap(const_cast<uint32_t*>(base_), pl_regs::kSpan);
    ::close(fd_);
  }
  uint32_t read(uint32_t offset) override { return base_[offset / 4]; }
  void write(uint32_t offset, uint32_t value) override {
    base_[offset / 4] = value;
  }

 private:
  int fd_ = -1;
  volatile uint32_t* base_ = nullptr;
};

uint32_t pack(int lo, int hi) {
  return static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
}

}  // namespace

std::unique_ptr<RegisterBank> open_uio_registers(const std::string& device) {
  return std::make_unique<UioRegisters>(device);
}

PlScrubber::PlScrubber(std::unique_ptr<RegisterBank> regs, int width,
                       int height, int max_age_frames)
    : regs_(std::move(regs)), width_(width), height_(height) {
  if (width <= 0 || height <= 0 || width > 0xFFFF || height > 0xFFFF ||
      max_age_frames < 0 || max_age_frames > 0xFFFF)
    throw std::invalid_argument("invalid PL scrubber geometry or max age");
  regs_->write(pl_regs::kWidth, width);
  regs_->write(pl_regs::kHeight, height);
  regs_->write(pl_regs::kFillY, 16);
  regs_->write(pl_regs::kFillC, 128);
  regs_->write(pl_regs::kMaxAgeFrames, max_age_frames);
  // Start from whatever sequence is in the register; the hardware only
  // trusts boxes after it sees commit_seq change.
  seq_ = regs_->read(pl_regs::kCommitSeq);
}

bool PlScrubber::commit(const std::vector<cv::Rect>& regions) {
  if (regs_->read(pl_regs::kLatchedSeq) != seq_) return false;

  std::vector<cv::Rect> boxes(regions.begin(), regions.end());
  if (boxes.size() > kMaxBoxes) {
    cv::Rect merged = boxes[kMaxBoxes - 1];
    for (size_t i = kMaxBoxes; i < boxes.size(); ++i) merged |= boxes[i];
    boxes.resize(kMaxBoxes);
    boxes.back() = merged;
  }
  const cv::Rect bounds(0, 0, width_, height_);
  for (const cv::Rect& r : boxes)
    if (r.empty() || (r & bounds) != r)
      throw std::invalid_argument("PL scrubber region out of bounds");

  regs_->write(pl_regs::kNumBoxes, static_cast<uint32_t>(boxes.size()));
  for (size_t i = 0; i < boxes.size(); ++i) {
    const cv::Rect& r = boxes[i];
    regs_->write(pl_regs::kBoxX[i], pack(r.x, r.x + r.width));
    regs_->write(pl_regs::kBoxY[i], pack(r.y, r.y + r.height));
  }
  // Last: changing the sequence is what makes the boxes visible to hardware.
  regs_->write(pl_regs::kCommitSeq, ++seq_);
  return true;
}

PlStatus PlScrubber::status() {
  return {regs_->read(pl_regs::kFrames), regs_->read(pl_regs::kBlankedFrames),
          regs_->read(pl_regs::kFramesSinceCommit)};
}

}  // namespace faceshield
