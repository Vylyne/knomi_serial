#include <Arduino.h>
#include <USB.h>
#include <Wire.h>

#include "board_conf.h"
#include "diag/i2c_scan.h"
#include "printer/config.h"
#include "printer/printer.h"
#include "printer/recv/recv_task.h"
#include "printer/send/send_task.h"
#include "ui/ui_task.h"
#include "user_conf.h"

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.setTimeout(SERIAL_TIMEOUT);
  Wire.begin(I2C0_SDA_PIN, I2C0_SCL_PIN, I2C0_SPEED);

  // Compiled out unless built with the knomi_i2cscan env. Deliberately here,
  // before any task exists, so nothing else can be using Wire.
  diag::i2c_scan();

  // Before the tasks, because after them both read config and neither may be
  // racing the load. It means the first frame the display ever draws is already
  // in the user's colours rather than the firmware's defaults.
  printer::config::begin();

  xTaskCreate(
    ui::ui_task,
    "ui",
    16384,
    NULL,
    10,
    NULL
  );

  xTaskCreate(
    printer::recv::recv_task,
    "recv",
    4096,
    NULL,
    9,
    NULL
  );

  xTaskCreate(
    printer::send::send_task,
    "send",
    4096,
    NULL,
    9,
    NULL
  );
}

void loop() {}