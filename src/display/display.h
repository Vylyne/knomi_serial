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

}

#endif