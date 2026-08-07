#ifndef I2C_SCAN_H
#define I2C_SCAN_H

#include <Arduino.h>
#include <Wire.h>

// Bench tool. Compiled out entirely unless built with -DI2C_SCAN=1, which the
// knomi_i2cscan env does.
//
// Answers two questions that the schematic can only suggest: whether the U6
// external connector really is the same bus the touch panel sits on, and
// whether the LIS2DW12 footprint is populated on this particular board.
//
// Runs from setup() before any task exists, so nothing else can be touching
// Wire at the time. A reset re-scans; there is deliberately no background
// polling, because sharing the bus with the touch driver from a second task is
// a race not worth introducing for a diagnostic.
namespace diag {

#if defined(I2C_SCAN) && I2C_SCAN

//: Parts this board might plausibly be carrying, so a hit reads as an answer
//: rather than as a number to go and look up.
inline const char *_guess(uint8_t addr) {
  if (addr == 0x15) return "CST816S touch";
  if (addr == 0x18 || addr == 0x19) return "LIS2DW12 accelerometer";
  if (addr >= 0x20 && addr <= 0x27) return "PCF8575/PCF8574 expander";
  if (addr >= 0x38 && addr <= 0x3f) return "PCF8574A expander";
  if (addr == 0x3c || addr == 0x3d) return "PCF8574A expander or SSD1306";
  return "unknown";
}

inline void i2c_scan() {
  // Prefixed like a device command so the host ignores it safely and
  // scripts/simulate.py prints it, rather than it being visible only to
  // whoever happens to have a serial monitor open.
  Serial.printf(
      "KNOMI_CMD:I2C:scan sda=%d scl=%d\n", I2C0_SDA_PIN, I2C0_SCL_PIN);

  int found = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      found++;
      Serial.printf("KNOMI_CMD:I2C:0x%02X %s\n", addr, _guess(addr));
    }
  }

  if (found == 0) {
    Serial.println("KNOMI_CMD:I2C:none - check pullups and wiring");
  } else {
    Serial.printf("KNOMI_CMD:I2C:done %d device(s)\n", found);
  }
}

#else

inline void i2c_scan() {}

#endif

}

#endif
