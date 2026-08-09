#include "init_screen.h"

#include "ui/theme.h"
#include "ui/ui.h"
#include "user_conf.h"

namespace ui
{
  namespace init_screen
  {

    static lv_obj_t *name = nullptr;
    static lv_obj_t *_spinner = nullptr;
    static lv_obj_t *_message = nullptr;

    void _printer_update_handler(const printer::State &state);

    lv_obj_t *init(const printer::State &state_in)
    {
      lv_obj_t *scr = lv_obj_create(nullptr);
      lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
      // A bare screen is scrollable, and one with anything near its edges can
      // be dragged around by a thumb. The page screens set a horizontal scroll
      // direction so they only ever move the way they are meant to; these
      // standalone ones had nothing saying so and slid in both.
      lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
      control::register_printer_update_cb(scr, _printer_update_handler);

      // The machine names itself at the top, and the state it is in sits at the
      // bottom under the link mark - so the two things you read are the two
      // ends, with what is happening in between.
      name = lv_label_create(scr);
      lv_label_set_text(name, PRINTER_NAME);
      lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 18);

      // In the machine's own colour, which is why config is loaded from flash
      // before any of this runs. Nothing has spoken to the host yet at this
      // point and nothing may for minutes, so a spinner that waited for the
      // wire would be the default pink on somebody else's printer for exactly
      // as long as it was the only thing on screen.
      _spinner = lv_spinner_create(scr);
      lv_obj_set_size(_spinner, 56, 56);
      lv_obj_align(_spinner, LV_ALIGN_CENTER, 0, -8);
      lv_obj_set_style_arc_width(_spinner, 5, LV_PART_MAIN);
      lv_obj_set_style_arc_width(_spinner, 5, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(_spinner, theme::machine(), LV_PART_INDICATOR);

      // Given a width, so it wraps. Without one a label is a single line as
      // long as its text, and this one carries whatever the host last said -
      // "MCU 'MCU' SHUTDOWN: LOST COMMUNICATION" ran off both sides of a round
      // screen at once.
      //
      // It takes the spinner's place rather than sitting under it. There is
      // only one clear area on this screen big enough for three lines, and if
      // we know why the host is gone that is worth more than an animation
      // saying we are still waiting - which the label at the bottom says
      // anyway.
      _message = lv_label_create(scr);
      lv_label_set_long_mode(_message, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(_message, 170);
      lv_label_set_text(_message, "");
      lv_obj_set_style_text_align(_message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(_message, LV_ALIGN_CENTER, 0, -8);
      lv_obj_add_flag(_message, LV_OBJ_FLAG_HIDDEN);

      lv_obj_t *state = lv_label_create(scr);
      lv_label_set_text(state, "WAITING");
      lv_obj_align(state, LV_ALIGN_BOTTOM_MID, 0, -12);

      _printer_update_handler(state_in);
      return scr;
    }

    void _printer_update_handler(const printer::State &state)
    {
      // Anything the host had to say before it went away - a shutdown reason,
      // or a local fault like a malformed frame. Nothing generates a message
      // here; this is the last one that arrived, which is the only account of
      // why there is no host to talk to.
      bool has_message = state.message[0] != '\0';
      if (has_message)
      {
        lv_label_set_text(_message, state.message);
      }
      if (has_message)
      {
        lv_obj_remove_flag(_message, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        lv_obj_add_flag(_message, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
      }
    }

  }
}