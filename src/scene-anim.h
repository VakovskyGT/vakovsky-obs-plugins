/* Program/preview scene tracking and the moment of the last program scene switch (frontend events). */
#pragma once

#include <stdint.h>

#include "copy-registry.h"

struct vf_anim_state {
	char program_uuid[VF_UUID_LEN], preview_uuid[VF_UUID_LEN], from_uuid[VF_UUID_LEN];
	uint64_t start_ns;               /* 0: no switch yet */
	uint32_t transition_ms;          /* duration of the switching transition; 0 for fixed ones (Cut) */
	int move_easing, move_easing_fn; /* Move's curve for matched items (vf_ease_move); -1: not Move */
};

/* What the module does at these moments; any member may be NULL. */
struct vf_anim_hooks {
	/* a transition starts between these scenes (either may be NULL) */
	void (*switch_started)(obs_source_t *from, const char *from_uuid, obs_source_t *dest, const char *dest_uuid);
	/* the transition stopped, or the scenes are going away (exit, collection cleanup or reload) */
	void (*switch_stopped)(void);
	/* a scene collection has loaded (FINISHED_LOADING, SCENE_COLLECTION_CHANGED) */
	void (*collection_loaded)(void);
	/* the program or preview scene changed, or Studio Mode went on or off; vf_anim_get already sees it. UI thread. */
	void (*scene_changed)(void);
};

void vf_anim_init(const struct vf_anim_hooks *hooks); /* copied; NULL: no hooks */
void vf_anim_free(void);
void vf_anim_get(struct vf_anim_state *out); /* any thread */
