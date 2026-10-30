/* Which profile a render shows, and the scene-switch animation run. Pure. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vf-params.h"

struct vf_select_in {
	bool per_scene;
	bool matched;              /* the rendered copy was identified (exact or scaled match) */
	const char *copy_scene;    /* uuid of the identified copy's scene */
	const char *program_scene; /* uuid */
	const char *anim_from, *anim_to;
	float anim_t; /* eased progress 0..1; < 0 when no animation runs */
	bool source_in_program;
	bool transition_active; /* the OBS transition of the last switch still runs (it may outlast the animation) */
};

enum vf_select_kind { VF_SEL_SCENE = 0, VF_SEL_DEFAULT = 1, VF_SEL_LERP = 2 };

struct vf_select_out {
	enum vf_select_kind kind;
	const char *scene; /* SCENE: the profile; LERP: the animation target */
};

struct vf_select_out vf_select(const struct vf_select_in *in);
/* The profile the program shows, independent of which copy is rendered: drives the reported source size. */
struct vf_select_out vf_select_program(const struct vf_select_in *in);

/* One scene-switch animation as seen by one filter: starts from what was shown when the switch happened,
 * so a switch during a running animation continues smoothly. */
struct vf_anim_run {
	uint64_t start_ns;
	struct vf_params from;
};

/* Starts a new run when start_ns differs from the current one; returns true if it did. */
bool vf_anim_run_sync(struct vf_anim_run *run, uint64_t start_ns, const struct vf_params *last_shown);
void vf_anim_run_value(const struct vf_anim_run *run, const struct vf_params *target, float t, struct vf_params *out);

/* Reorders n ids (strings in fixed slots of len bytes) to follow order: ids found there come first, in that order;
 * the rest keep their relative order after them. Lists scenes the way the OBS Scenes dock does. */
void vf_order_like(char *ids, size_t n, size_t len, const char *const *order, size_t m);

/* Whether a copy counts as being in its scene for a switch: a hidden item (or one in a hidden group) is not there, Move
 * shows it appearing, so nothing animates from that scene's look. */
bool vf_copy_shown(bool item_visible, bool in_group, bool group_visible);
