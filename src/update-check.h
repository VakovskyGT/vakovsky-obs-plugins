/* Once a day, in a background thread, asks GitHub whether this module has a newer release; the result is cached in
 * the module's config (update.json) and shown in the filter properties (vf-about.c). Nothing is downloaded. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The default of "enabled" in update.json when it has none yet (no UI: set it to false there to stop checking). */
#define VF_UPDATE_DEFAULT_ON true

void vf_update_start(const char *tag_prefix); /* obs_module_post_load: when enabled, checks whenever a day has passed */
void vf_update_stop(void);                    /* obs_module_unload: wakes the thread, aborts a running request, joins */
void vf_update_skip(void); /* UI thread: hides the known release until a newer one comes out; writes the config */
/* True and the release when a newer, not skipped one is known and the check is on; copied under the lock. */
bool vf_update_available(char *version, size_t version_len, char *url, size_t url_len);
