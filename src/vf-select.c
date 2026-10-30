#include "vf-select.h"

#include <stdlib.h>
#include <string.h>

static bool same(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

struct vf_select_out vf_select(const struct vf_select_in *in)
{
	struct vf_select_out out = {VF_SEL_DEFAULT, NULL};

	if (!in->per_scene)
		return out;

	if ((in->anim_t >= 0.0f || in->transition_active) && !in->source_in_program && in->anim_from &&
	    in->anim_from[0] && (!in->matched || same(in->copy_scene, in->anim_from))) {
		/* leaving the program: keep the old scene's look while it goes */
		out.kind = VF_SEL_SCENE;
		out.scene = in->anim_from;
		return out;
	}

	if (in->anim_t >= 0.0f &&
	    (!in->matched || same(in->copy_scene, in->anim_from) || same(in->copy_scene, in->anim_to))) {
		out.kind = VF_SEL_LERP;
		out.scene = in->anim_to;
		return out;
	}

	if (in->matched) {
		out.kind = VF_SEL_SCENE;
		out.scene = in->copy_scene;
	} else if (in->source_in_program) {
		out.kind = VF_SEL_SCENE;
		out.scene = in->program_scene;
	}
	return out;
}

bool vf_anim_run_sync(struct vf_anim_run *run, uint64_t start_ns, const struct vf_params *last_shown)
{
	if (run->start_ns == start_ns)
		return false;
	run->start_ns = start_ns;
	run->from = *last_shown;
	return true;
}

void vf_anim_run_value(const struct vf_anim_run *run, const struct vf_params *target, float t, struct vf_params *out)
{
	vf_params_lerp(&run->from, target, t, out);
}

struct vf_select_out vf_select_program(const struct vf_select_in *in)
{
	struct vf_select_in p = *in;
	p.matched = false;
	p.copy_scene = NULL;
	return vf_select(&p);
}

void vf_order_like(char *ids, size_t n, size_t len, const char *const *order, size_t m)
{
	char *out = malloc(n * len);
	bool *taken = calloc(n, sizeof(bool));
	if (!out || !taken) {
		free(out);
		free(taken);
		return;
	}
	size_t k = 0;
	for (size_t j = 0; j < m; j++) {
		for (size_t i = 0; i < n; i++) {
			if (!taken[i] && strcmp(ids + i * len, order[j]) == 0) {
				memcpy(out + k++ * len, ids + i * len, len);
				taken[i] = true;
			}
		}
	}
	for (size_t i = 0; i < n; i++)
		if (!taken[i])
			memcpy(out + k++ * len, ids + i * len, len);
	memcpy(ids, out, n * len);
	free(out);
	free(taken);
}

bool vf_copy_shown(bool item_visible, bool in_group, bool group_visible)
{
	return item_visible && (!in_group || group_visible);
}
