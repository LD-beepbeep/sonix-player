#ifndef WEATHERPAGE_H
#define WEATHERPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *weather_screen;

void weatherpage_init(gui_config_t *cfg);

#endif
