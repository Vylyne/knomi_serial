#ifndef BUTTON_INPUT_H
#define BUTTON_INPUT_H

#include <lvgl.h>
#include <stdint.h>

#include "printer/printer.h"

namespace input {
namespace button {

enum class Phase : uint8_t {
  kPress,
  kRelease,
  kCancel,
};

typedef void (*page_action_t)(Phase phase, void *context);

void begin();
bool enqueue(uint8_t index, bool pressed, uint32_t config_crc);

//: UI-task entry points. Dispatch can touch LVGL and therefore never runs in
//: recv or GPIO producer tasks.
void drain();
void sync_config();
void cancel_all();

bool register_page_action(
    lv_obj_t *owner, printer::ButtonSlot slot,
    page_action_t action, void *context = nullptr);
void clear_page_actions();
bool slot_claimed(printer::ButtonSlot slot);
bool slot_touch_enabled(printer::ButtonSlot slot);
void page_event(lv_obj_t *owner, printer::ButtonSlot slot, Phase phase);

bool profile_active(printer::ButtonProfile profile);
void build_overlay(lv_obj_t *parent);
void set_overlay_touch_enabled(bool enabled);
void set_overlay_touch_compact(bool compact);

}
}

#endif
