#include <stdint.h>
#include <stdio.h>

#include "ui/contrast.h"

int main() {
  struct Case {
    uint32_t rgb;
    bool black;
  };
  const Case cases[] = {
      {0x000000, false}, {0xffffff, true}, {0xffa7c4, true},
      {0xff0000, true}, {0x0000ff, false},
      {0x757575, false}, {0x767676, true},
  };
  ui::contrast::AccentInkCache palette;
  for (const Case &sample : cases) {
    if (!palette.update(sample.rgb) || palette.use_black() != sample.black) {
      fprintf(stderr, "wrong ink for %06lx\n", (unsigned long)sample.rgb);
      return 1;
    }
    if (palette.update(sample.rgb)) {
      fprintf(stderr, "recomputed unchanged accent %06lx\n", (unsigned long)sample.rgb);
      return 1;
    }
  }
  return 0;
}
