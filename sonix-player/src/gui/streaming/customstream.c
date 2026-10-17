#include "customstream.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/radio.h"

typedef struct {
	char name[RADIO_NAME_MAX];
	char url[RADIO_URL_MAX];
} service_stream_t;

lv_obj_t *bandcamp_screen;
lv_obj_t *soundcloud_screen;

#define SERVICE_MAX 64

static service_stream_t bandcamp_streams[SERVICE_MAX];
static service_stream_t soundcloud_streams[SERVICE_MAX];
static int bandcamp_count;
static int soundcloud_count;

static char sd_root[512];

static lv_obj_t *bandcamp_list;
static lv_obj_t *soundcloud_list;

static const char *trim_left(const char *s) {
	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	return s;
}

static void trim_right(char *s) {
	int len = (int)strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) {
		s[--len] = '\0';
	}
}

static int read_streams(const char *path, service_stream_t *out, int max) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}

	char line[1024];
	int count = 0;
	while (fgets(line, sizeof(line), f) && count < max) {
		trim_right(line);
		const char *start = trim_left(line);
		if (!start[0] || start[0] == '#') {
			continue;
		}

		char *comma = strchr((char *)start, ',');
		if (!comma) {
			continue;
		}
		*comma = '\0';
		char *name = (char *)trim_left(start);
		trim_right(name);
		char *url = (char *)trim_left(comma + 1);
		trim_right(url);
		if (!name[0] || !url[0]) {
			continue;
		}

		snprintf(out[count].name, sizeof(out[count].name), "%s", name);
		snprintf(out[count].url, sizeof(out[count].url), "%s", url);
		count++;
	}
	fclose(f);
	return count;
}

static void play_stream(const service_stream_t *stream) {
	radio_station_t station;
	memset(&station, 0, sizeof(station));
	snprintf(station.name, sizeof(station.name), "%s", stream->name);
	snprintf(station.url, sizeof(station.url), "%s", stream->url);

	if (!radio_play(&station)) {
		const char *why = radio_station_problem(&station);
		gui_notify_popup(why && why[0] ? why : "streaming_unavailable");
		return;
	}
	player_sheet_open(true);
}

static void stream_row_cb(lv_event_t *e) {
	intptr_t packed = (intptr_t)lv_event_get_user_data(e);
	int service = (int)((packed >> 16) & 0xffff);
	int index = (int)(packed & 0xffff);

	if (service == 1) {
		if (index >= 0 && index < bandcamp_count) {
			play_stream(&bandcamp_streams[index]);
		}
		return;
	}
	if (service == 2) {
		if (index >= 0 && index < soundcloud_count) {
			play_stream(&soundcloud_streams[index]);
		}
	}
}

static void add_dynamic_row(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data) {
	lv_obj_t *row = cb ? settingsrow_action(parent, "", cb, user_data) : settingsrow_add(parent, "", NULL, NULL, NULL);
	lv_obj_t *name = settingsrow_name_label(row);
	lv_label_set_text(name, text);
	settingsrow_name_lines(row, 2);
}

static void rebuild_list(lv_obj_t *list, service_stream_t *streams, int count, const char *file_hint_key, int service) {
	lv_obj_clean(list);
	add_dynamic_row(list, tr(file_hint_key), NULL, NULL);
	if (count <= 0) {
		add_dynamic_row(list, tr("custom_links_empty"), NULL, NULL);
		return;
	}

	for (int i = 0; i < count; i++) {
		intptr_t packed = ((intptr_t)service << 16) | (intptr_t)i;
		add_dynamic_row(list, streams[i].name, stream_row_cb, (void *)packed);
	}
}

static void bandcamp_loaded_cb(lv_event_t *e) {
	(void)e;
	char path[768];
	snprintf(path, sizeof(path), "%s/bandcamp.txt", sd_root);
	bandcamp_count = read_streams(path, bandcamp_streams, SERVICE_MAX);
	rebuild_list(bandcamp_list, bandcamp_streams, bandcamp_count, "bandcamp_links_hint", 1);
}

static void soundcloud_loaded_cb(lv_event_t *e) {
	(void)e;
	char path[768];
	snprintf(path, sizeof(path), "%s/soundcloud.txt", sd_root);
	soundcloud_count = read_streams(path, soundcloud_streams, SERVICE_MAX);
	rebuild_list(soundcloud_list, soundcloud_streams, soundcloud_count, "soundcloud_links_hint", 2);
}

void customstream_init(gui_config_t *cfg) {
	snprintf(sd_root, sizeof(sd_root), "%s", cfg->sd_root_path ? cfg->sd_root_path : "");

	bandcamp_list = settingsrow_page(bandcamp_screen, cfg, "bandcamp");
	lv_obj_add_event_cb(bandcamp_screen, bandcamp_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	soundcloud_list = settingsrow_page(soundcloud_screen, cfg, "soundcloud");
	lv_obj_add_event_cb(soundcloud_screen, soundcloud_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
