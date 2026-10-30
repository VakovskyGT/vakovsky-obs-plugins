#include "update-check.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "vf-update.h"

#define RELEASES_URL "https://api.github.com/repos/VakovskyGT/vakovsky-obs-plugins/releases?per_page=30"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static bool thread_running;
static volatile long stopping;
static os_event_t *stop_event; /* wakes the waiting thread when OBS closes */
static char prefix[64];
static bool enabled = VF_UPDATE_DEFAULT_ON;
static char known_version[32], known_url[512];
static char skipped[32]; /* the release the user chose to skip */

/* caller holds lock */
static obs_data_t *config_load(void)
{
	char *path = obs_module_config_path("update.json");
	obs_data_t *d = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	bfree(path);
	return d ? d : obs_data_create();
}

/* caller holds lock */
static void config_save(obs_data_t *d)
{
	char *dir = obs_module_config_path("");
	char *path = obs_module_config_path("update.json");
	if (dir && path) {
		os_mkdirs(dir);
		obs_data_save_json_safe(d, path, "tmp", "bak");
	}
	bfree(path);
	bfree(dir);
}

/* caller holds lock: a cached release counts only while it is newer than this build and the skipped one */
static void remember(const char *version, const char *url)
{
	struct vf_version cur;
	if (vf_version_parse(PLUGIN_VERSION, &cur) && vf_update_shown(version, skipped, cur)) {
		snprintf(known_version, sizeof(known_version), "%s", version);
		snprintf(known_url, sizeof(known_url), "%s", url ? url : "");
	} else {
		known_version[0] = known_url[0] = '\0';
	}
}

static size_t on_data(char *ptr, size_t size, size_t nmemb, void *param)
{
	struct dstr *body = param;
	if (body->len > 4 * 1024 * 1024)
		return 0; /* not a release list */
	dstr_ncat(body, ptr, size * nmemb);
	return size * nmemb;
}

static int on_progress(void *param, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
	UNUSED_PARAMETER(c);
	UNUSED_PARAMETER(d);
	return os_atomic_load_long(&stopping) ? 1 : 0; /* non-zero aborts: OBS is closing */
}

/* Returns the HTTP status (0 when the request failed) and fills body. */
static long fetch(struct dstr *body)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return 0;
	struct dstr agent = {0};
	dstr_printf(&agent, "%s/%s", PLUGIN_NAME, PLUGIN_VERSION);
	struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
	curl_easy_setopt(curl, CURLOPT_URL, RELEASES_URL);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.array);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
#if LIBCURL_VERSION_NUM >= 0x075700
	/* closing OBS must not wait for a stuck DNS lookup to finish (OBS never unloads modules mid-run) */
	curl_easy_setopt(curl, CURLOPT_QUICK_EXIT, 1L);
#endif
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
	long status = 0;
	if (curl_easy_perform(curl) == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	dstr_free(&agent);
	return status;
}

static bool check_due(void)
{
	pthread_mutex_lock(&lock);
	obs_data_t *cfg = config_load();
	bool due = vf_update_due(obs_data_get_int(cfg, "last_check"), (int64_t)time(NULL));
	obs_data_release(cfg);
	pthread_mutex_unlock(&lock);
	return due;
}

static void check_once(void)
{
	struct dstr body = {0};
	long status = fetch(&body);
	int64_t now = (int64_t)time(NULL);
	if (os_atomic_load_long(&stopping)) {
		dstr_free(&body);
		return;
	}

	struct vf_version cur = {0, 0, 0};
	vf_version_parse(PLUGIN_VERSION, &cur);
	char version[32] = "", url[512] = "";
	bool newer = status == 200 &&
		     vf_update_pick(body.array, prefix, cur, version, sizeof(version), url, sizeof(url));
	if (status == 200)
		obs_log(LOG_INFO, "update check: %s", newer ? version : "up to date");
	else
		obs_log(LOG_INFO, "update check failed (HTTP %ld)", status);
	dstr_free(&body);

	pthread_mutex_lock(&lock);
	obs_data_t *cfg = config_load();
	obs_data_set_int(cfg, "last_check", now); /* a failed attempt also waits a day */
	if (status == 200) {
		obs_data_set_string(cfg, "version", version);
		obs_data_set_string(cfg, "url", url);
		remember(version, url);
	}
	config_save(cfg);
	obs_data_release(cfg);
	pthread_mutex_unlock(&lock);
}

/* OBS may run for days: look again every hour whether a day has passed (another OBS may have checked meanwhile) */
static void *check_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("vf-update-check");
	do {
		if (check_due())
			check_once();
	} while (!os_atomic_load_long(&stopping) && os_event_timedwait(stop_event, 3600 * 1000) == ETIMEDOUT);
	return NULL;
}

void vf_update_start(const char *tag_prefix)
{
	snprintf(prefix, sizeof(prefix), "%s", tag_prefix);
	os_atomic_set_long(&stopping, 0);

	/* the switch and a release found earlier are known at once; only the request runs in the background */
	pthread_mutex_lock(&lock);
	obs_data_t *cfg = config_load();
	obs_data_set_default_bool(cfg, "enabled", VF_UPDATE_DEFAULT_ON);
	enabled = obs_data_get_bool(cfg, "enabled");
	snprintf(skipped, sizeof(skipped), "%s", obs_data_get_string(cfg, "skip"));
	remember(obs_data_get_string(cfg, "version"), obs_data_get_string(cfg, "url"));
	obs_data_release(cfg);
	pthread_mutex_unlock(&lock);
	if (!enabled || os_event_init(&stop_event, OS_EVENT_TYPE_MANUAL) != 0)
		return;

	curl_global_init(CURL_GLOBAL_DEFAULT);
	thread_running = pthread_create(&thread, NULL, check_thread, NULL) == 0;
	if (!thread_running) {
		curl_global_cleanup();
		os_event_destroy(stop_event);
		stop_event = NULL;
	}
}

void vf_update_stop(void)
{
	os_atomic_set_long(&stopping, 1);
	if (thread_running) {
		os_event_signal(stop_event);
		pthread_join(thread, NULL);
		curl_global_cleanup();
		os_event_destroy(stop_event);
		stop_event = NULL;
	}
	thread_running = false;
}

void vf_update_skip(void)
{
	pthread_mutex_lock(&lock);
	if (known_version[0]) {
		snprintf(skipped, sizeof(skipped), "%s", known_version);
		known_version[0] = known_url[0] = '\0';
		obs_data_t *cfg = config_load();
		obs_data_set_string(cfg, "skip", skipped);
		config_save(cfg);
		obs_data_release(cfg);
	}
	pthread_mutex_unlock(&lock);
}

bool vf_update_available(char *version, size_t version_len, char *url, size_t url_len)
{
	pthread_mutex_lock(&lock);
	bool have = enabled && known_version[0];
	if (have) {
		snprintf(version, version_len, "%s", known_version);
		snprintf(url, url_len, "%s", known_url);
	}
	pthread_mutex_unlock(&lock);
	return have;
}
