#include "vf-fade.h"

#include <math.h>

#define VF_FADE_BUCKET 64.0f

/* Margin along one axis in transition px, kept small enough for the texture limit, and the texture size. The
 * texture grows in 64 px buckets so an item Move scales a little every frame keeps its texture; the quad then
 * covers the whole texture (a slightly wider margin is harmless). */
static void fit_axis(uint32_t size, float margin, float scale, float *m, float *quad, uint32_t *tex)
{
	const float max_tex = (float)VF_FADE_MAX_TEX;
	float mm = fmaxf(margin, 0.0f) / scale;
	float room = ((max_tex - VF_FADE_BUCKET) / scale - (float)size) * 0.5f;
	mm = fminf(mm, fmaxf(room, 0.0f));

	float px = ((float)size + 2.0f * mm) * scale;
	px = ceilf(px / VF_FADE_BUCKET - 1e-4f) * VF_FADE_BUCKET;
	px = fminf(fmaxf(px, VF_FADE_BUCKET), max_tex);

	*m = mm;
	*tex = (uint32_t)px;
	*quad = px / scale;
}

void vf_fade_layout(uint32_t child_w, uint32_t child_h, uint32_t tr_w, uint32_t tr_h, float margin, float canvas_sx,
		    float canvas_sy, struct vf_fade_layout *out)
{
	if (!(canvas_sx > 1e-4f) || !(canvas_sy > 1e-4f))
		canvas_sx = canvas_sy = 1.0f;

	fit_axis(tr_w, margin, canvas_sx, &out->mx, &out->quad_w, &out->tex_w);
	fit_axis(tr_h, margin, canvas_sy, &out->my, &out->quad_h, &out->tex_h);
	out->sx = out->sy = 1.0f;
	out->ox = out->oy = 0.0f;

	if (!child_w || !child_h || (child_w == tr_w && child_h == tr_h))
		return;

	float s = fminf((float)tr_w / (float)child_w, (float)tr_h / (float)child_h);
	out->sx = out->sy = s;
	out->ox = ((float)tr_w - (float)child_w * s) * 0.5f;
	out->oy = ((float)tr_h - (float)child_h * s) * 0.5f;
}

#ifdef _MSC_VER
static __declspec(thread) const char *render_scene;
#else
static _Thread_local const char *render_scene;
#endif

const char *vf_fade_scene_push(const char *scene_uuid)
{
	const char *previous = render_scene;
	render_scene = scene_uuid;
	return previous;
}

const char *vf_fade_scene_get(void)
{
	return render_scene;
}
