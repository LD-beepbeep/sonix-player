#include "rsspage.h"

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"

lv_obj_t *rss_screen;
lv_obj_t *rss_feed_screen;

#define RSS_FEEDS_MAX 40
#define RSS_HEADLINES_MAX 20

typedef struct {
	char name[96];
	char url[512];
} rss_feed_t;

static char sd_root[512];

static rss_feed_t feeds[RSS_FEEDS_MAX];
static int feed_count;
static int selected_feed = -1;

static char headlines[RSS_HEADLINES_MAX][180];
static int headline_count;
static bool headline_ok;
static bool worker_running;
static unsigned request_token;

static lv_obj_t *feeds_list;
static lv_obj_t *headlines_list;
static lv_obj_t *feed_title;

typedef struct {
	unsigned token;
	bool ok;
	int count;
	char lines[RSS_HEADLINES_MAX][180];
} rss_result_t;

typedef struct {
	unsigned token;
	char url[512];
} rss_job_t;

static void trim_right(char *s) {
	int len = (int)strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) {
		s[--len] = '\0';
	}
}

static const char *trim_left(const char *s) {
	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	return s;
}

static void decode_entities(char *text) {
	char out[180];
	int j = 0;
	for (int i = 0; text[i] && j < (int)sizeof(out) - 1; i++) {
		if (strncmp(text + i, "&amp;", 5) == 0) {
			out[j++] = '&';
			i += 4;
		} else if (strncmp(text + i, "&lt;", 4) == 0) {
			out[j++] = '<';
			i += 3;
		} else if (strncmp(text + i, "&gt;", 4) == 0) {
			out[j++] = '>';
			i += 3;
		} else if (strncmp(text + i, "&quot;", 6) == 0) {
			out[j++] = '"';
			i += 5;
		} else if (strncmp(text + i, "&#39;", 5) == 0) {
			out[j++] = '\'';
			i += 4;
		} else {
			out[j++] = text[i];
		}
	}
	out[j] = '\0';
	snprintf(text, 180, "%s", out);
}

static int parse_titles(const char *xml, char out[RSS_HEADLINES_MAX][180]) {
	int count = 0;
	const char *p = xml;

	while (count < RSS_HEADLINES_MAX) {
		const char *item = strstr(p, "<item");
		const char *entry = strstr(p, "<entry");
		const char *start = NULL;
		if (item && entry) {
			start = item < entry ? item : entry;
		} else {
			start = item ? item : entry;
		}
		if (!start) {
			break;
		}

		const char *title = strstr(start, "<title");
		if (!title) {
			p = start + 5;
			continue;
		}
		title = strchr(title, '>');
		if (!title) {
			break;
		}
		title++;
		const char *end = strstr(title, "</title>");
		if (!end) {
			break;
		}

		size_t n = (size_t)(end - title);
		if (n >= 179) {
			n = 179;
		}
		memcpy(out[count], title, n);
		out[count][n] = '\0';

		if (strncmp(out[count], "<![CDATA[", 9) == 0) {
			char *cend = strstr(out[count], "]]>");
			if (cend) {
				*cend = '\0';
				memmove(out[count], out[count] + 9, strlen(out[count] + 9) + 1);
			}
		}
		decode_entities(out[count]);
		trim_right(out[count]);
		if (out[count][0]) {
			count++;
		}
		p = end + 8;
	}
	return count;
}

static int read_feeds(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	char line[1024];
	int count = 0;
	while (fgets(line, sizeof(line), f) && count < RSS_FEEDS_MAX) {
		trim_right(line);
		const char *s = trim_left(line);
		if (!s[0] || s[0] == '#') {
			continue;
		}
		char *comma = strchr((char *)s, ',');
		if (!comma) {
			continue;
		}
		*comma = '\0';
		char *name = (char *)trim_left(s);
		trim_right(name);
		char *url = (char *)trim_left(comma + 1);
		trim_right(url);
		if (!name[0] || !url[0]) {
			continue;
		}
		snprintf(feeds[count].name, sizeof(feeds[count].name), "%s", name);
		snprintf(feeds[count].url, sizeof(feeds[count].url), "%s", url);
		count++;
	}
	fclose(f);
	return count;
}

static lv_obj_t *add_dynamic_row(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data) {
	lv_obj_t *row = cb ? settingsrow_action(parent, "", cb, user_data) : settingsrow_add(parent, "", NULL, NULL, NULL);
	lv_obj_t *name = settingsrow_name_label(row);
	lv_label_set_text(name, text);
	settingsrow_name_lines(row, 2);
	return row;
}

static void refresh_headlines_view(void) {
	lv_obj_clean(headlines_list);
	if (selected_feed < 0 || selected_feed >= feed_count) {
		add_dynamic_row(headlines_list, tr("rss_no_feed_selected"), NULL, NULL);
		return;
	}
	lv_label_set_text(feed_title, feeds[selected_feed].name);

	if (worker_running) {
		add_dynamic_row(headlines_list, tr("rss_loading"), NULL, NULL);
		return;
	}
	if (!headline_ok || headline_count <= 0) {
		add_dynamic_row(headlines_list, tr("rss_no_headlines"), NULL, NULL);
		return;
	}
	for (int i = 0; i < headline_count; i++) {
		add_dynamic_row(headlines_list, headlines[i], NULL, NULL);
	}
}

static void feed_select_cb(lv_event_t *e);

static void refresh_feeds_view(void) {
	lv_obj_clean(feeds_list);
	add_dynamic_row(feeds_list, tr("rss_links_hint"), NULL, NULL);
	if (feed_count <= 0) {
		add_dynamic_row(feeds_list, tr("rss_no_links"), NULL, NULL);
		return;
	}
	for (int i = 0; i < feed_count; i++) {
		add_dynamic_row(feeds_list, feeds[i].name, feed_select_cb, (void *)(intptr_t)i);
	}
}

static void fetch_result_cb(void *user_data) {
	rss_result_t *result = user_data;
	worker_running = false;
	if (result->token == request_token) {
		headline_ok = result->ok;
		headline_count = result->count;
		for (int i = 0; i < headline_count; i++) {
			snprintf(headlines[i], sizeof(headlines[i]), "%s", result->lines[i]);
		}
	}
	refresh_headlines_view();
	free(result);
}

static void *fetch_worker(void *user_data) {
	rss_job_t *job = user_data;
	rss_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		worker_running = false;
		free(job);
		return NULL;
	}
	result->token = job->token;

	char *body = NULL;
	size_t len = 0;
	if (http_get_ex(job->url, "User-Agent: " HTTP_USER_AGENT "\r\n", false, &body, &len, 1024 * 1024, 20, NULL) && body && len > 0) {
		result->count = parse_titles(body, result->lines);
		result->ok = result->count > 0;
	}
	free(body);
	free(job);

	if (!gui_post(fetch_result_cb, result)) {
		free(result);
		worker_running = false;
	}
	return NULL;
}

static void fetch_selected_feed(void) {
	if (selected_feed < 0 || selected_feed >= feed_count || worker_running) {
		return;
	}
	worker_running = true;
	request_token++;
	refresh_headlines_view();

	rss_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		worker_running = false;
		return;
	}
	job->token = request_token;
	snprintf(job->url, sizeof(job->url), "%s", feeds[selected_feed].url);

	pthread_t thread;
	if (pthread_create(&thread, NULL, fetch_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		free(job);
		worker_running = false;
		headline_ok = false;
		headline_count = 0;
		refresh_headlines_view();
	}
}

static void feed_select_cb(lv_event_t *e) {
	selected_feed = (int)(intptr_t)lv_event_get_user_data(e);
	headline_count = 0;
	headline_ok = false;
	switch_screen(rss_feed_screen);
	fetch_selected_feed();
}

static void feed_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_headlines_view();
	if (!worker_running && headline_count == 0) {
		fetch_selected_feed();
	}
}

static void feeds_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	char path[768];
	snprintf(path, sizeof(path), "%s/rss.txt", sd_root);
	feed_count = read_feeds(path);
	refresh_feeds_view();
}

void rsspage_init(gui_config_t *cfg) {
	snprintf(sd_root, sizeof(sd_root), "%s", cfg->sd_root_path ? cfg->sd_root_path : "");

	feeds_list = settingsrow_page(rss_screen, cfg, "rss_reader");
	lv_obj_add_event_cb(rss_screen, feeds_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	headlines_list = settingsrow_page(rss_feed_screen, cfg, "rss_headlines");
	feed_title = settingsrow_page_title(rss_feed_screen);
	lv_obj_add_event_cb(rss_feed_screen, feed_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
