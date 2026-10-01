#include <stdint.h>
#include <stdio.h>

#include "display/capture_coverage.h"

int main() {
  uint8_t seen[2] = {};
  display::CaptureCoverage coverage(4, 3);
  coverage.reset(seen);
  coverage.mark(0, 0, 3, 0);
  coverage.mark(0, 0, 3, 0);
  coverage.mark(0, 0, 3, 0);
  if (coverage.complete() || coverage.covered() != 4) {
    fprintf(stderr, "overlapping flushes falsely completed the frame\n");
    return 1;
  }
  coverage.mark(0, 1, 3, 2, false);
  if (coverage.complete()) {
    fprintf(stderr, "completed before LVGL's final flush\n");
    return 1;
  }
  coverage.mark(0, 1, 3, 2, true);
  if (!coverage.complete() || coverage.covered() != 12) {
    fprintf(stderr, "full frame did not complete\n");
    return 1;
  }
  coverage.reset(seen);
  if (coverage.complete() || coverage.covered() != 0) {
    fprintf(stderr, "reset retained old coverage\n");
    return 1;
  }
  return 0;
}
