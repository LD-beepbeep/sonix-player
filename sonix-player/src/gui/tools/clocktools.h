#ifndef CLOCKTOOLS_H
#define CLOCKTOOLS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *clocktools_screen;
extern lv_obj_t *alarmset_screen;

void clocktools_init(gui_config_t *cfg);

#endif
