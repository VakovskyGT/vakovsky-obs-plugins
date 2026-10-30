#include "vf-match.h"

#include <math.h>
#include <string.h>

static float axis_len(const float *row)
{
	return sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
}

static float rel_diff(float a, float b)
{
	return fabsf(a - b) / fmaxf(1.0f, fabsf(b));
}

/* Error of rendering this copy directly: the render matrix equals its draw_transform. */
static float exact_direct_err(const float m[16], const struct vf_copy *c)
{
	float e = 0.0f;
	for (int i = 0; i < 16; i++)
		e = fmaxf(e, rel_diff(m[i], c->dt[i]));
	return e;
}

/* Error of rendering this copy into OBS's item texture: scale w/cx, h/cy and an offset by the crop. */
static float exact_texpath_err(const float m[16], uint32_t w, uint32_t h, const struct vf_copy *c)
{
	float cx = (float)w - (float)(c->crop_l + c->crop_r);
	float cy = (float)h - (float)(c->crop_t + c->crop_b);
	if (cx <= 0.0f || cy <= 0.0f)
		return INFINITY;

	float ex = (float)w / cx, ey = (float)h / cy;
	float l = (float)c->crop_l, t = (float)c->crop_t;
	float scaled_offset = fabsf(m[12] + l * ex) + fabsf(m[13] + t * ey);
	float plain_offset = fabsf(m[12] + l) + fabsf(m[13] + t);
	float off = fminf(scaled_offset, plain_offset) / fmaxf(1.0f, l + t);

	return fmaxf(fmaxf(fabsf(axis_len(&m[0]) - ex), fabsf(axis_len(&m[4]) - ey)), off);
}

/* Error of rendering this copy under an unknown uniform scale + offset (Studio Mode preview, multiview). */
static float scaled_err(const float m[16], const struct vf_copy *c)
{
	float len = axis_len(&c->dt[0]);
	if (len < 1e-6f)
		return INFINITY;

	float s = axis_len(&m[0]) / len;
	static const int idx[6] = {0, 1, 2, 4, 5, 6};
	float e = 0.0f;
	for (int k = 0; k < 6; k++)
		e = fmaxf(e, rel_diff(m[idx[k]], s * c->dt[idx[k]]));
	return e;
}

static bool better(const struct vf_copy *copies, int cand, int best)
{
	return best < 0 || copies[cand].rank < copies[best].rank;
}

struct vf_match_result vf_match_copy(const float m[16], uint32_t src_w, uint32_t src_h, const struct vf_copy *copies,
				     size_t n, bool allow_scaled)
{
	struct vf_match_result res = {-1, VF_MATCH_NONE, INFINITY};

	for (size_t i = 0; i < n; i++) {
		const struct vf_copy *c = &copies[i];
		if (c->hidden)
			continue;
		float e = c->texpath ? exact_texpath_err(m, src_w, src_h, c) : exact_direct_err(m, c);
		if (e <= VF_MATCH_EPS && better(copies, (int)i, res.index)) {
			res.index = (int)i;
			res.kind = VF_MATCH_EXACT;
			res.err = e;
		}
	}
	if (res.index >= 0 || !allow_scaled)
		return res;

	for (size_t i = 0; i < n; i++) {
		const struct vf_copy *c = &copies[i];
		if (c->texpath || c->hidden)
			continue;
		float e = scaled_err(m, c);
		if (e <= VF_MATCH_EPS && better(copies, (int)i, res.index)) {
			res.index = (int)i;
			res.kind = VF_MATCH_SCALED;
			res.err = e;
		}
	}
	return res;
}

void vf_copy_scale(const struct vf_copy *c, float *sx, float *sy)
{
	*sx = axis_len(&c->dt[0]);
	*sy = axis_len(&c->dt[4]);
}

void vf_copy_scale_moving(const struct vf_copy *from, const struct vf_copy *to, float t, float *sx, float *sy)
{
	float fx, fy, tx, ty;
	vf_copy_scale(from ? from : to, &fx, &fy);
	vf_copy_scale(to ? to : from, &tx, &ty);
	t = fminf(fmaxf(t, 0.0f), 1.0f);
	*sx = fx + (tx - fx) * t;
	*sy = fy + (ty - fy) * t;
}

static bool same_scene(const char *a, const char *b)
{
	return a && b && *b && strcmp(a, b) == 0;
}

int vf_copy_rank(const char *scene, const char *program, const char *from, const char *preview, bool window_render)
{
	const bool is_program = same_scene(scene, program);
	const bool is_preview = same_scene(scene, preview);
	const bool is_from = same_scene(scene, from);
	if (window_render && preview && *preview)
		return is_preview ? 0 : is_program ? 1 : is_from ? 2 : 3;
	return is_program ? 0 : is_from ? 1 : is_preview ? 2 : 3;
}

int vf_copy_rank_scoped(int rank, const char *scene, const char *scope)
{
	return same_scene(scene, scope) ? -1 : rank;
}
