/* Update check logic: versions, the newest own release in GitHub's release list, when to check. Pure apart from
 * obs_data for JSON. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vf_version {
	int major, minor, patch;
};

bool vf_version_parse(const char *text, struct vf_version *out); /* exactly "1.2.3" */
int vf_version_cmp(struct vf_version a, struct vf_version b);    /* <0, 0, >0 */
/* Newest release that is neither a draft nor a prerelease, whose tag is <tag_prefix><x.y.z>, newer than current.
 * json is GitHub's releases array. Writes its version text and html_url; false if there is none. */
bool vf_update_pick(const char *json, const char *tag_prefix, struct vf_version current, char *version,
		    size_t version_len, char *url, size_t url_len);
/* At most once a day; a last check in the future (the clock went back) counts as due. */
bool vf_update_due(int64_t last_check_unix, int64_t now_unix);
/* A known release is shown while it is newer than this build and than the release the user skipped (may be ""). */
bool vf_update_shown(const char *version, const char *skipped, struct vf_version current);
