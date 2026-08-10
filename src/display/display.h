#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

namespace display {

void init();
void destroy();
void set_backlight(uint8_t target);

//: Flushes, pixels pushed and microseconds spent since the last call, then
//: resets. Pixels per second is what says whether a screen is affordable: a
//: whole frame is 57600 of them and takes over 11ms on this panel's SPI clock.
void take_flush_stats(uint32_t *count, uint32_t *pixels, uint32_t *micros_spent);

// Grabbing what is on the glass, for documentation.
//
// Taken from the flush callback rather than with lv_snapshot: every pixel that
// reaches the panel passes through there already, so this needs no second
// render pass, no LV_USE_SNAPSHOT, and no draw buffer carved out of LVGL's
// 64k arena - and it captures exactly what was displayed rather than a
// re-rendering of what should have been. The frame lands in PSRAM, which is
// 8MB of otherwise unused memory on this part.
//
// Call from the LVGL task only.

//: Start capturing, and invalidate the screen so a whole frame is produced.
//: False if PSRAM could not be had.
bool capture_begin();

//: Whether a full frame has been collected since capture_begin.
bool capture_complete();

//: Stop writing into the buffer but keep it, so the frame can be read out at
//: leisure while the UI carries on drawing. Without this, sending would be
//: racing the flush callback for the same memory.
void capture_freeze();

//: The captured frame: RES_H*RES_V pixels, native-endian RGB565, or null.
//: Valid until the next capture_begin.
const uint16_t *capture_frame();

//: Release the buffer and stop capturing.
void capture_end();

}

#endif