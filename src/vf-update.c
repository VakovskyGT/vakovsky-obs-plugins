#include "vf-update.h"

#include <obs-data.h>
#include <util/dstr.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_part(const char **p, int *out)
{
	if (!isdigit((unsigned char)**p))
		return false;
	char *end;
	long v = strtol(*p, &end, 10);
	if (v < 0 || v > 1000000)
		return false;
	*out = (int)v;
	*p = end;
	return true;
}

bool vf_version_parse(const char *text, struct vf_version *out)
{
	const char *p = text;
	struct vf_version v;
	if (!p || !parse_part(&p, &v.major) || *p++ != '.' || !parse_part(&p, &v.minor) || *p++ != '.' ||
	    !parse_part(&p, &v.patch) || *p)
		return false;
	*out = v;
	return true;
}

int vf_version_cmp(struct vf_version a, struct vf_version b)
{
	if (a.major != b.major)
		return a.major < b.major ? -1 : 1;
	if (a.minor != b.minor)
		return a.minor < b.minor ? -1 : 1;
	if (a.patch != b.patch)
		return a.patch < b.patch ? -1 : 1;
	return 0;
}

bool vf_update_pick(const char *json, const char *tag_prefix, struct vf_version current, char *version,
		    size_t version_len, char *url, size_t url_len)
{
	if (!json || !*json || !tag_prefix)
		return false;
	/* obs_data reads objects only: the array goes into a wrapper */
	struct dstr wrapped = {0};
	dstr_printf(&wrapped, "{\"r\":%s}", json);
	obs_data_t *root = obs_data_create_from_json(wrapped.array);
	dstr_free(&wrapped);
	obs_data_array_t *arr = root ? obs_data_get_array(root, "r") : NULL;

	bool found = false;
	struct vf_version best = current;
	size_t prefix_len = strlen(tag_prefix);
	for (size_t i = 0; arr && i < obs_data_array_count(arr); i++) {
		obs_data_t *rel = obs_data_array_item(arr, i);
		const char *tag = obs_data_get_string(rel, "tag_name");
		struct vf_version v;
		if (!obs_data_get_bool(rel, "draft") && !obs_data_get_bool(rel, "prerelease") &&
		    strncmp(tag, tag_prefix, prefix_len) == 0 && vf_version_parse(tag + prefix_len, &v) &&
		    vf_version_cmp(v, best) > 0) {
			best = v;
			found = true;
			snprintf(version, version_len, "%s", tag + prefix_len);
			snprintf(url, url_len, "%s", obs_data_get_string(rel, "html_url"));
		}
		obs_data_release(rel);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
	return found;
}

bool vf_update_due(int64_t last_check_unix, int64_t now_unix)
{
	return last_check_unix > now_unix || now_unix - last_check_unix >= 24 * 3600;
}

bool vf_update_shown(const char *version, const char *skipped, struct vf_version current)
{
	struct vf_version v, s;
	if (!version || !vf_version_parse(version, &v) || vf_version_cmp(v, current) <= 0)
		return false;
	return !skipped || !vf_version_parse(skipped, &s) || vf_version_cmp(v, s) > 0;
}
