#ifndef DISPLAY_CAPTURE_COVERAGE_H
#define DISPLAY_CAPTURE_COVERAGE_H

#include <stdint.h>
#include <string.h>

namespace display {

// LVGL may flush the same area repeatedly while a full redraw is pending.
// Counting flushed pixels can therefore finish a screenshot with holes in it.
class CaptureCoverage {
 public:
  CaptureCoverage(uint16_t width, uint16_t height)
      : width_(width), height_(height) {}

  uint32_t bytes() const {
    return ((uint32_t)width_ * height_ + 7) / 8;
  }

  void reset(uint8_t *seen) {
    seen_ = seen;
    covered_ = 0;
    last_flush_ = false;
    memset(seen_, 0, bytes());
  }

  void mark(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
            bool last_flush = false) {
    for (uint32_t y = y1; y <= y2; ++y) {
      for (uint32_t x = x1; x <= x2; ++x) {
        uint32_t index = y * width_ + x;
        uint8_t mask = (uint8_t)(1u << (index & 7));
        uint8_t &flags = seen_[index >> 3];
        if (!(flags & mask)) {
          flags |= mask;
          ++covered_;
        }
      }
    }
    last_flush_ = last_flush;
  }

  uint32_t covered() const { return covered_; }
  bool complete() const {
    return last_flush_ && covered_ == (uint32_t)width_ * height_;
  }

 private:
  uint16_t width_;
  uint16_t height_;
  uint8_t *seen_ = nullptr;
  uint32_t covered_ = 0;
  bool last_flush_ = false;
};

}

#endif
