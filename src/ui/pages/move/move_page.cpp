#include "move_page.h"

#include <stdint.h>

#include "board_conf.h"
#include "input/button_input.h"
#include "printer/send/send_cmd.h"
#include "ui/action_control.h"
#include "ui/screens/screen_helper.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace move_page {

namespace {

enum Direction : uint8_t { kXm, kXp, kYp, kYm, kZp, kZm };

struct MoveAction {
  Direction direction;
  int32_t x;
  int32_t y;
  const char *symbol;
};

constexpr MoveAction kActions[] = {
    {kXm, -100, 0, LV_SYMBOL_LEFT "X"},
    {kXp, 40, 0, "X" LV_SYMBOL_RIGHT},
    {kYp, -30, -70, "Y" LV_SYMBOL_UP},
    {kYm, -30, 70, "Y" LV_SYMBOL_DOWN},
    {kZp, 70, -70, "Z" LV_SYMBOL_UP},
    {kZm, 70, 70, "Z" LV_SYMBOL_DOWN},
};

// A shared legend at either right corner takes the outer top/bottom positions.
// Keep Z on the right edge, but bring the pair together around the X row.
constexpr int32_t kSharedZOffset = 92;
constexpr int32_t kSharedZRowOffset = 25;
constexpr int32_t kSharedZTargetSize = 46;

static_assert(kActions[kXm].x + kActions[kXp].x == 2 * kActions[kYp].x
                  && kActions[kYp].x == kActions[kYm].x,
              "Move X controls must balance around the Y column");
static_assert(kActions[kYp].y == kActions[kZp].y
                  && kActions[kYm].y == kActions[kZm].y,
              "Move Y and Z controls must align in top and bottom rows");
static_assert(kSharedZOffset - kSharedZTargetSize / 2 >
                  kActions[kXp].x + MOVE_TARGET_SIZE / 2,
              "Shifted Z touch targets must clear X+ on the right edge");
static_assert(2 * kSharedZRowOffset > kSharedZTargetSize,
              "Shifted Z touch targets must not overlap each other");
static_assert(CORNER_OFFSET - kSharedZRowOffset >
                  (CORNER_SIZE + kSharedZTargetSize) / 2,
              "Shifted Z touch targets must clear the corner legends");
static_assert(kSharedZOffset * kSharedZOffset +
                  kSharedZRowOffset * kSharedZRowOffset <=
                  (RES_H / 2 - MOVE_MARK_SIZE / 2) *
                  (RES_H / 2 - MOVE_MARK_SIZE / 2),
              "Shifted Z marks must fit within the round display");

void _move(input::button::Phase phase, void *context) {
  if (phase != input::button::Phase::kRelease) {
    return;
  }
  switch ((Direction)(uintptr_t)context) {
  case kXm: printer::send::send_move("X-"); break;
  case kXp: printer::send::send_move("X+"); break;
  case kYp: printer::send::send_move("Y+"); break;
  case kYm: printer::send::send_move("Y-"); break;
  case kZp: printer::send::send_move("Z+"); break;
  case kZm: printer::send::send_move("Z-"); break;
  }
}

}

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  (void)state;
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);
  screen_helper::register_overlay_exclusion(page);

  bool shift_z = input::button::slot_claimed(printer::ButtonSlot::kNE) ||
      input::button::slot_claimed(printer::ButtonSlot::kSE);

  for (const MoveAction &action : kActions) {
    bool is_z = action.direction == kZp || action.direction == kZm;
    int32_t target_size = is_z
        ? (shift_z ? kSharedZTargetSize : 50) : MOVE_TARGET_SIZE;
    int32_t x = shift_z && is_z ? kSharedZOffset : action.x;
    int32_t y = shift_z && is_z
        ? (action.direction == kZp ? -kSharedZRowOffset : kSharedZRowOffset)
        : action.y;
    action_control::create(
        page, x, y, target_size, MOVE_MARK_SIZE,
        action.symbol, &lv_font_montserrat_16, theme::machine(), true,
        printer::ButtonSlot::kNone, _move,
        (void *)(uintptr_t)action.direction);
  }
  return page;
}

void printer_update(const printer::State &state) {
  (void)state;
}

}
}
