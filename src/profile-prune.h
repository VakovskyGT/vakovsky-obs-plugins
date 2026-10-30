/* Per-scene settings of deleted scenes are dropped when a scene collection has loaded: until then undo can bring a
 * deleted scene back with the same uuid, and it must find its profile. */
#pragma once

#include <obs.h>

/* Every filter with this unversioned id gets prune() with the uuids of the existing scenes. UI thread. */
void vf_prune_profiles_of(const char *filter_id,
			  void (*prune)(obs_source_t *filter, const char *const *known, size_t n));

/* fn() for every filter with this unversioned id. UI thread. */
void vf_for_each_filter(const char *filter_id, void (*fn)(obs_source_t *filter));

/* Each filter drops the scene profiles whose uuid is not in known (frame-filter.c, size-filter.c). UI thread. */
void vf_frame_prune(obs_source_t *filter, const char *const *known, size_t n);
void vf_size_prune(obs_source_t *filter, const char *const *known, size_t n);
