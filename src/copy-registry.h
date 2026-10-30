/* Plugin-wide table "source -> its scene item copies", rebuilt only when OBS signals a change. */
#pragma once

#include <obs.h>

#include "vf-match.h"

#define VF_UUID_LEN 40

struct vf_reg_copy {
	obs_sceneitem_t *item;  /* addref'd until the next rebuild */
	obs_sceneitem_t *group; /* group item containing it, or NULL */
	char scene_uuid[VF_UUID_LEN];
};

void vf_registry_init(void);
void vf_registry_free(void);
void vf_registry_tick(void);
void vf_registry_mark_dirty(void);
uint64_t vf_registry_generation(void);

/* Graphics thread only; the array stays valid until the next vf_registry_tick(). */
size_t vf_registry_get(obs_source_t *source, const struct vf_reg_copy **out);

/* Whether the copy is on screen in its scene: the item, and the group holding it, visible (vf_copy_shown). */
bool vf_reg_copy_shown(const struct vf_reg_copy *c);

/* Any thread (the tick runs outside the graphics context): whether the source has shown copies in scenes a and b. */
void vf_registry_in_scenes(obs_source_t *source, const char *a, const char *b, bool *in_a, bool *in_b);

/* Any thread: copies the unique scene uuids containing the source into out (at most max). */
size_t vf_registry_scene_uuids(obs_source_t *source, char (*out)[VF_UUID_LEN], size_t max);

/* Any thread: scene uuids where a copy of the source is rendered through OBS's item texture (crop, blending...). */
size_t vf_registry_texture_scenes(obs_source_t *source, char (*out)[VF_UUID_LEN], size_t max);

/* Fills the matching descriptor from the item's live transform/crop. */
void vf_reg_copy_fill(const struct vf_reg_copy *c, uint32_t src_w, uint32_t src_h, int rank, struct vf_copy *out);
