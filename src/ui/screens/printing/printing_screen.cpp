#include "printing_screen.h"

#include "ui/screens/screen_helper.h"
#include "ui/pages/estop/estop_page.h"
#include "ui/pages/printing/printing_page.h"
#include "ui/ui.h"

namespace ui {
namespace printing_screen {

void _printer_update_handler(const printer::State &state);

//: In build order - tag_pages stamps them that way and update_visible indexes
//: by the stamp.
const screen_helper::page_update_t _updates[] = {
    printing_page::printer_update,
    estop_page::printer_update,
};

lv_obj_t *_scr = nullptr;

lv_obj_t *init(const printer::State &state) {
  lv_obj_t *scr = screen_helper::create_screen();
  _scr = scr;
  control::register_printer_update_cb(scr, _printer_update_handler);

  printing_page::init(scr, state);
  estop_page::init(scr, state);

  screen_helper::tag_pages(scr);
  return scr;
}

void _printer_update_handler(const printer::State &state) {
  screen_helper::update_visible(
      _scr, state, _updates, sizeof(_updates) / sizeof(_updates[0]));
}

}
}