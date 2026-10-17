#include "weatherpage.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"

lv_obj_t *weather_screen;

static char sd_root[512];
static lv_obj_t *weather_text;
static lv_obj_t *location_value;
static bool worker_running;

typedef struct {
	bool ok;
	char line[192];
	char location[96];
} weather_result_t;

static void trim_right(char *s) {
	int len = (int)strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) {
		s[--len] = '\0';
	}
}

static void read_location(char *out, size_t out_size) {
	out[0] = '\0';
	char path[768];
	snprintf(path, sizeof(path), "%s/weather.txt", sd_root);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	if (fgets(out, (int)out_size, f)) {
		trim_right(out);
	}
	fclose(f);
}

static void weather_result_cb(void *user) {
	weather_result_t *result = user;
	worker_running = false;
	if (location_value) {
		lv_label_set_text(location_value, result->location[0] ? result->location : tr("weather_auto"));
	}
	if (weather_text) {
		lv_label_set_text(weather_text, result->ok ? result->line : tr("weather_unavailable"));
	}
	free(result);
}

static void *weather_worker(void *unused) {
	(void)unused;
	weather_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		worker_running = false;
		return NULL;
	}

	read_location(result->location, sizeof(result->location));
	char encoded[192];
	http_url_encode(result->location, encoded, sizeof(encoded));

	char url[320];
	if (encoded[0]) {
		snprintf(url, sizeof(url), "https://wttr.in/%s?format=3", encoded);
	} else {
		snprintf(url, sizeof(url), "https://wttr.in/?format=3");
	}

	char *body = NULL;
	size_t len = 0;
	if (http_get_ex(url, "User-Agent: " HTTP_USER_AGENT "\r\n", false, &body, &len, 4096, 15, NULL) && body && len > 0) {
		trim_right(body);
		snprintf(result->line, sizeof(result->line), "%s", body);
		result->ok = true;
	}
	free(body);

	if (!gui_post(weather_result_cb, result)) {
		free(result);
		worker_running = false;
	}
	return NULL;
}

static void weather_refresh(void) {
	if (worker_running) {
		return;
	}
	worker_running = true;
	lv_label_set_text(weather_text, tr("weather_loading"));

	pthread_t thread;
	if (pthread_create(&thread, NULL, weather_worker, NULL) == 0) {
		pthread_detach(thread);
	} else {
		worker_running = false;
		lv_label_set_text(weather_text, tr("weather_unavailable"));
	}
}

static void refresh_row_cb(lv_event_t *e) {
	(void)e;
	weather_refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	weather_refresh();
}

void weatherpage_init(gui_config_t *cfg) {
	snprintf(sd_root, sizeof(sd_root), "%s", cfg->sd_root_path ? cfg->sd_root_path : "");

	lv_obj_t *container = settingsrow_page(weather_screen, cfg, "weather");
	settingsrow_add(container, "weather_location", &location_value, NULL, NULL);
	settingsrow_action(container, "refresh", refresh_row_cb, NULL);

	weather_text = lv_label_create(container);
	lv_obj_add_style(weather_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(weather_text, &font_ui_24, 0);
	lv_obj_set_width(weather_text, lv_pct(100));
	lv_label_set_long_mode(weather_text, LV_LABEL_LONG_WRAP);
	lv_label_set_text(weather_text, tr("weather_loading"));

	lv_obj_add_event_cb(weather_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
