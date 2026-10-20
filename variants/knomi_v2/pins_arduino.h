#ifndef Pins_Arduino_h
#define Pins_Arduino_h

// Arduino's default pins for the Knomi V2. See docs/hardware.md.
//
// These are what the core falls back to when a caller passes no pins - a bare
// Wire.begin(), Serial1 with no arguments. The firmware's own pin names live in
// src/knomi_v2.h, and tests/test_variant.py keeps the two in agreement.
//
// There is deliberately no variant.cpp. A board's initVariant() runs before
// setup(), and nothing on this board needs driving that early.
//
// No LED_BUILTIN or RGB_BUILTIN: the board has neither. No USB_VID/USB_PID:
// the S3's USB pins drive the panel, so the only USB device is the CH340K. No
// A or T pins: nothing here reads an analog or touch input.

#include <stdint.h>

// UART0, to the CH340K.
static const uint8_t TX = 43;
static const uint8_t RX = 44;

// I2C bus 0: the touch panel and the external U6 port.
static const uint8_t SDA = 2;
static const uint8_t SCL = 1;

// The panel's SPI bus. MISO is wired as LCD-SDO but nothing reads it.
static const uint8_t SS   = 20;
static const uint8_t MOSI = 14;
static const uint8_t MISO = 13;
static const uint8_t SCK  = 18;

#endif /* Pins_Arduino_h */
