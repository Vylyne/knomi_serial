#include "gpio_input.h"

#include <Arduino.h>
#include <string.h>

#include "input/button_input.h"
#include "printer/config.h"
#include "printer/printer.h"
#include "user_conf.h"

namespace input {
namespace gpio {

namespace {

struct PinState {
  uint8_t pin;
  bool configured;
  bool raw_pressed;
  bool stable_pressed;
  uint32_t changed_at;
};

PinState _pins[printer::kMaxButtons] = {};
uint32_t _config_crc = 0;
bool _configured = false;

void _release_pins() {
  for (PinState &state : _pins) {
    if (state.configured) {
      // Remove our pull-up when this record stops owning the pin. INPUT leaves
      // it high impedance instead of silently continuing to bias shared wiring.
      pinMode(state.pin, INPUT);
    }
  }
  memset(_pins, 0, sizeof(_pins));
}

void _configure(uint32_t now, const printer::Config &conf) {
  _release_pins();
  for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
    const printer::ButtonConfig &binding = conf.buttons[i];
    if (binding.source == printer::ButtonSource::kNone) {
      break;
    }
    if (binding.source != printer::ButtonSource::kGpio) {
      continue;
    }

    PinState &state = _pins[i];
    state.pin = binding.pin;
    state.configured = true;
    // Buttons close to ground. The internal pull-up gives an unpressed HIGH
    // without requiring an external resistor; LOW is therefore pressed.
    pinMode(state.pin, INPUT_PULLUP);
    state.raw_pressed = digitalRead(state.pin) == LOW;
    state.stable_pressed = false;
    state.changed_at = now;
  }
}

}

void gpio_task(void *param) {
  (void)param;
  while (true) {
    uint32_t now = millis();
    uint32_t crc = printer::config::held_crc();
    if (!_configured || crc != _config_crc) {
      printer::Config conf;
      printer::config::snapshot(&conf, &crc);
      _configured = true;
      _config_crc = crc;
      _configure(now, conf);
    }

    for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
      PinState &state = _pins[i];
      if (!state.configured) {
        continue;
      }
      bool pressed = digitalRead(state.pin) == LOW;
      if (pressed != state.raw_pressed) {
        state.raw_pressed = pressed;
        state.changed_at = now;
      } else if (pressed != state.stable_pressed &&
                 (uint32_t)(now - state.changed_at) >= GPIO_DEBOUNCE_MS) {
        // A full queue must not consume the edge. Retry on the next poll so a
        // release cannot be permanently lost while the UI is busy.
        if (input::button::enqueue(i, pressed, _config_crc)) {
          state.stable_pressed = pressed;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(GPIO_POLL_MS));
  }
}

}
}
