#include "clocktools.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/system/core/lang.h"

lv_obj_t *clocktools_screen;
lv_obj_t *alarmset_screen;

static lv_obj_t *stopwatch_value;
static lv_obj_t *timer_value;
static lv_obj_t *alarm_value;
static lv_obj_t *alarm_toggle;
static lv_obj_t *start_stop_row;

static settingsrow_duration_t timer_duration;
static lv_timer_t *tick_timer;

static bool stopwatch_running;
static uint32_t stopwatch_started_ms;
static uint32_t stopwatch_accum_ms;

static bool timer_running;
static uint32_t timer_deadline_ms;

static bool alarm_enabled;
static int alarm_hour = 7;
static int alarm_minute;
static int alarm_last_stamp = -1;

static lv_obj_t *alarm_hour_roller;
static lv_obj_t *alarm_minute_roller;

static void fmt_hms(uint32_t total_seconds, char *out, size_t out_size) {
	uint32_t hours = total_seconds / 3600;
	uint32_t mins = (total_seconds / 60) % 60;
	uint32_t secs = total_seconds % 60;
	if (hours > 0) {
		snprintf(out, out_size, "%u:%02u:%02u", (unsigned)hours, (unsigned)mins, (unsigned)secs);
	} else {
		snprintf(out, out_size, "%02u:%02u", (unsigned)mins, (unsigned)secs);
	}
}

static void refresh_alarm_value(void) {
	char text[16];
	snprintf(text, sizeof(text), "%02d:%02d", alarm_hour, alarm_minute);
	lv_label_set_text(alarm_value, text);
}

static void refresh_start_stop_label(void) {
	lv_obj_t *name = settingsrow_name_label(start_stop_row);
	lv_label_set_text(name, tr(stopwatch_running ? "clock_stop" : "clock_start"));
}

static void refresh_stopwatch_value(void) {
	uint32_t elapsed = stopwatch_accum_ms;
	if (stopwatch_running) {
		elapsed += lv_tick_elaps(stopwatch_started_ms);
	}
	char text[32];
	fmt_hms(elapsed / 1000U, text, sizeof(text));
	lv_label_set_text(stopwatch_value, text);
}

static void refresh_timer_value(void) {
	if (!timer_running) {
		lv_label_set_text(timer_value, tr("clock_timer_idle"));
		return;
	}
	uint32_t now = lv_tick_get();
	uint32_t remain_ms = (int32_t)(timer_deadline_ms - now) > 0 ? (timer_deadline_ms - now) : 0;
	char text[32];
	fmt_hms(remain_ms / 1000U, text, sizeof(text));
	lv_label_set_text(timer_value, text);
}

static void start_stopwatch_cb(lv_event_t *e) {
	(void)e;
	if (stopwatch_running) {
		stopwatch_accum_ms += lv_tick_elaps(stopwatch_started_ms);
		stopwatch_running = false;
	} else {
		stopwatch_started_ms = lv_tick_get();
		stopwatch_running = true;
	}
	refresh_start_stop_label();
	refresh_stopwatch_value();
}

static void reset_stopwatch_cb(lv_event_t *e) {
	(void)e;
	stopwatch_running = false;
	stopwatch_accum_ms = 0;
	refresh_start_stop_label();
	refresh_stopwatch_value();
}

static void timer_toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(timer_duration.toggle, LV_STATE_CHECKED);
	settingsrow_duration_expanded(&timer_duration, on);
	if (!on) {
		timer_running = false;
		refresh_timer_value();
		return;
	}
	int minutes = settingsrow_duration_minutes(&timer_duration);
	if (minutes <= 0) {
		minutes = 1;
		settingsrow_duration_set_minutes(&timer_duration, minutes);
	}
	timer_deadline_ms = lv_tick_get() + (uint32_t)minutes * 60U * 1000U;
	timer_running = true;
	refresh_timer_value();
}

static void timer_duration_cb(lv_event_t *e) {
	(void)e;
	if (!timer_running) {
		return;
	}
	int minutes = settingsrow_duration_minutes(&timer_duration);
	if (minutes <= 0) {
		minutes = 1;
	}
	timer_deadline_ms = lv_tick_get() + (uint32_t)minutes * 60U * 1000U;
	refresh_timer_value();
}

static void alarm_toggle_cb(lv_event_t *e) {
	(void)e;
	alarm_enabled = lv_obj_has_state(alarm_toggle, LV_STATE_CHECKED);
}

static void alarm_save_cb(lv_event_t *e) {
	(void)e;
	alarm_hour = lv_roller_get_selected(alarm_hour_roller);
	alarm_minute = lv_roller_get_selected(alarm_minute_roller);
	refresh_alarm_value();
	switch_screen(clocktools_screen);
}

static void alarm_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	lv_roller_set_selected(alarm_hour_roller, (uint16_t)alarm_hour, LV_ANIM_OFF);
	lv_roller_set_selected(alarm_minute_roller, (uint16_t)alarm_minute, LV_ANIM_OFF);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_resume(tick_timer);
	refresh_stopwatch_value();
	refresh_timer_value();
	refresh_alarm_value();
	refresh_start_stop_label();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(tick_timer);
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;

	refresh_stopwatch_value();

	if (timer_running) {
		if ((int32_t)(lv_tick_get() - timer_deadline_ms) >= 0) {
			timer_running = false;
			lv_obj_remove_state(timer_duration.toggle, LV_STATE_CHECKED);
			settingsrow_duration_expanded(&timer_duration, false);
			gui_notify_popup("clock_timer_finished");
		}
		refresh_timer_value();
	}

	if (!alarm_enabled) {
		return;
	}

	time_t now = time(NULL);
	struct tm tm_now;
	localtime_r(&now, &tm_now);
	int stamp = tm_now.tm_yday * 1440 + tm_now.tm_hour * 60 + tm_now.tm_min;
	if (tm_now.tm_hour == alarm_hour && tm_now.tm_min == alarm_minute && stamp != alarm_last_stamp) {
		alarm_last_stamp = stamp;
		gui_notify_popup("clock_alarm_fired");
	}
}

void clocktools_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(clocktools_screen, cfg, "clock_tools");

	settingsrow_add(container, "clock_stopwatch", &stopwatch_value, NULL, NULL);
	start_stop_row = settingsrow_action(container, "clock_start", start_stopwatch_cb, NULL);
	settingsrow_action(container, "reset", reset_stopwatch_cb, NULL);

	settingsrow_toggle_duration(container, "clock_timer", timer_toggle_cb, timer_duration_cb, &timer_duration);
	settingsrow_duration_set_minutes(&timer_duration, 5);
	settingsrow_duration_expanded(&timer_duration, false);

	settingsrow_add(container, "clock_timer_remaining", &timer_value, NULL, NULL);
	settingsrow_toggle(container, "clock_alarm", &alarm_toggle, alarm_toggle_cb);
	settingsrow_add(container, "clock_alarm_time", &alarm_value, switch_screen_cb, alarmset_screen);

	lv_obj_t *alarm_container = settingsrow_page(alarmset_screen, cfg, "clock_alarm_time");
	alarm_hour_roller = lv_roller_create(alarm_container);
	lv_roller_set_options(alarm_hour_roller,
		"00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23",
		LV_ROLLER_MODE_NORMAL);
	lv_obj_set_width(alarm_hour_roller, lv_pct(48));

	alarm_minute_roller = lv_roller_create(alarm_container);
	lv_roller_set_options(alarm_minute_roller,
		"00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59",
		LV_ROLLER_MODE_NORMAL);
	lv_obj_set_width(alarm_minute_roller, lv_pct(48));

	settingsrow_action(alarm_container, "save", alarm_save_cb, NULL);

	tick_timer = lv_timer_create(tick_cb, 250, NULL);
	lv_timer_pause(tick_timer);

	lv_obj_add_event_cb(clocktools_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(clocktools_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(alarmset_screen, alarm_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	refresh_start_stop_label();
	refresh_stopwatch_value();
	refresh_timer_value();
	refresh_alarm_value();
}
