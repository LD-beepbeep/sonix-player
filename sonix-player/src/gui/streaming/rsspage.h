#ifndef RSSPAGE_H
#define RSSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *rss_screen;
extern lv_obj_t *rss_feed_screen;

void rsspage_init(gui_config_t *cfg);

#endif
