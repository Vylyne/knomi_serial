#include "printing_screen.h"

#include "ui/screens/screen_helper.h"
#include "ui/pages/estop/estop_page.h"
#include "ui/pages/printing/printing_page.h"
#include "ui/ui.h"

namespace ui {
namespace printing_screen {

void _printer_update_handler(const printer::State &state);

//: In build order - tag_pages stamps them that way and update_visible indexes
//: by the stamp. The e-stop is not here: it hangs below the row rather than in
//: it, the same as on the idle screen.
const screen_helper::page_update_t _updates[] = {
    printing_page::printer_update,
};

lv_obj_t *_scr = nullptr;

lv_obj_t *init(const printer::State &state) {
  lv_obj_t *scr = screen_helper::create_screen();
  lv_obj_t *row = screen_helper::page_row(scr);
  _scr = scr;
  control::register_printer_update_cb(scr, _printer_update_handler);

  printing_page::init(row, state);
  estop_page::init(screen_helper::estop_slot(scr), state);

  screen_helper::tag_pages(row);
  return scr;
}

void _printer_update_handler(const printer::State &state) {
  screen_helper::update_visible(
      screen_helper::page_row(_scr), state, _updates,
      sizeof(_updates) / sizeof(_updates[0]));
  estop_page::printer_update(state);
}

}
}