#include "vf-size.h"

#include <math.h>
#include <string.h>
#include "vf-params.h"

bool vf_size_anim_at(const struct vf_size_anim *a, uint64_t now_ns, float *box_w, float *box_h, long long *page_w,
		     long long *page_h, float *mix)
{
	double raw = a->ms && now_ns > a->start_ns ? (double)(now_ns - a->start_ns) / ((double)a->ms * 1e6)
						   : (a->ms ? 0.0 : 1.0);
	if (raw >= 1.0) {
		*box_w = (float)a->to_w;
		*box_h = (float)a->to_h;
		*page_w = a->to_w;
		*page_h = a->to_h;
		*mix = 1.0f;
		return false;
	}
	float t = a->easing < 0 || a->easing_fn <= 0 ? vf_ease(VF_EASE_SMOOTH, (float)raw)
						     : vf_ease_move(a->easing, a->easing_fn, (float)raw);
	*box_w = (float)a->from_w + (float)(a->to_w - a->from_w) * t;
	*box_h = (float)a->from_h + (float)(a->to_h - a->from_h) * t;
	*page_w = a->to_w;
	*page_h = a->to_h;
	*mix = t;
	return true;
}

void vf_size_pick_from(bool busy, float box_w, float box_h, bool have_stored, long long stored_w, long long stored_h,
		       long long cur_w, long long cur_h, long long *from_w, long long *from_h)
{
	if (busy) {
		*from_w = (long long)lroundf(box_w);
		*from_h = (long long)lroundf(box_h);
		return;
	}
	*from_w = have_stored ? stored_w : cur_w;
	*from_h = have_stored ? stored_h : cur_h;
}

bool vf_size_needs_update(long long set_w, long long set_h, long long real_w, long long real_h, long long w,
			  long long h)
{
	return set_w != w || set_h != h || real_w != w || real_h != h;
}

float vf_size_fade_alpha(uint64_t end_ns, uint64_t now_ns, uint64_t fade_ns)
{
	if (!fade_ns || now_ns >= end_ns + fade_ns)
		return 0.0f;
	if (now_ns <= end_ns)
		return 1.0f;
	return 1.0f - (float)((double)(now_ns - end_ns) / (double)fade_ns);
}

enum vf_size_switch vf_size_switch_kind(bool in_from, bool in_program, bool has_size, uint32_t ms, bool smooth)
{
	if (!in_program)
		return in_from ? VF_SW_KEEP : VF_SW_NONE;
	if (!has_size)
		return VF_SW_NONE;
	return in_from && smooth && ms > 0 ? VF_SW_ANIMATE : VF_SW_JUMP;
}

bool vf_size_anchor(int mode, float *x, float *y)
{
	if (mode < 1 || mode > 9) {
		*x = *y = 0.5f;
		return true;
	}
	*x = (float)((mode - 1) % 3) * 0.5f;
	*y = (float)((mode - 1) / 3) * 0.5f;
	return false;
}

uint32_t vf_size_page_height(bool fixed, bool moving, uint32_t requested, uint32_t working_height)
{
	return fixed && moving && working_height > requested ? working_height : requested;
}

uint64_t vf_size_settle_ns(bool dissolve, bool fixed_height, uint32_t new_h, uint32_t cur_h, uint64_t frame_ns)
{
	if (dissolve)
		return frame_ns * 2;
	return fixed_height && new_h != cur_h ? frame_ns * 3 : 0;
}

enum vf_size_fade_role vf_size_fade_role(bool active, const char *render_scene, const char *from, const char *to)
{
	if (!active || !render_scene || !*render_scene)
		return VF_SIZE_FADE_LIVE;
	if (from && strcmp(render_scene, from) == 0)
		return VF_SIZE_FADE_OLD;
	if (to && strcmp(render_scene, to) == 0)
		return VF_SIZE_FADE_NEW;
	return VF_SIZE_FADE_LIVE;
}

bool vf_size_resolve(long long w, long long h, uint32_t cur_w, uint32_t cur_h, long long *out_w, long long *out_h)
{
	if (w <= 0 && h <= 0)
		return false;
	*out_w = w > 0 ? w : (long long)cur_w;
	*out_h = h > 0 ? h : (long long)cur_h;
	return true;
}

long vf_size_to_image(long page_len, uint32_t page, uint32_t image)
{
	long v = page_len + ((long)image - (long)page);
	return v > 0 ? v : 1;
}

bool vf_size_profile_key(const char *name, size_t uuid_len, char *uuid)
{
	if ((name[0] != 'w' && name[0] != 'h') || name[1] != '_' || !name[2])
		return false;
	size_t n = strlen(name + 2);
	if (n >= uuid_len)
		return false;
	memcpy(uuid, name + 2, n + 1);
	return true;
}

bool vf_size_stale_key(const char *name, const char *const *known_uuids, size_t n)
{
	char uuid[64];
	if (!vf_size_profile_key(name, sizeof(uuid), uuid))
		return false;
	for (size_t i = 0; i < n; i++)
		if (strcmp(known_uuids[i], uuid) == 0)
			return false;
	return true;
}

static size_t find_id(const char *id, const char *ids, size_t stride, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (id && id[0] && strcmp(ids + i * stride, id) == 0)
			return i;
	return n;
}

size_t vf_size_edit_pick(const char *stored, const char *current, bool fresh, const char *ids, size_t stride, size_t n)
{
	size_t cur = find_id(current, ids, stride, n), kept = find_id(stored, ids, stride, n);
	if (fresh && cur < n)
		return cur;
	if (kept < n)
		return kept;
	return cur < n ? cur : 0;
}

const char *vf_size_capture_scene(const char *picked, const char *program)
{
	return picked && *picked ? picked : program;
}
