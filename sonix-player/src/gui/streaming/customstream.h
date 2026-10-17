#ifndef CUSTOMSTREAM_H
#define CUSTOMSTREAM_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *bandcamp_screen;
extern lv_obj_t *soundcloud_screen;

void customstream_init(gui_config_t *cfg);

#endif
