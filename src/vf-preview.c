#include "vf-preview.h"

#include <math.h>
#include <stdlib.h>

bool vf_preview_is_source_view(const struct vf_preview_in *in)
{
	const int vw = in->view_w, vh = in->view_h;
	if (vw == (int)in->src_w && vh == (int)in->src_h)
		return false;
	for (int i = 0; i < 16; i++)
		if (fabsf(in->m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) > 1e-4f)
			return false;
	if (in->match != VF_MATCH_EXACT)
		return true;
	if (!in->editor_open || (vw == (int)in->canvas_w && vh == (int)in->canvas_h))
		return false;
	/* the dialog fits the source into its window, each side rounded down */
	const long long d = (long long)vw * in->src_h - (long long)vh * in->src_w;
	return llabs(d) <= (long long)in->src_w + in->src_h;
}

void vf_preview_fit(float w, float h, float ext_x, float ext_y, float *k, float *tx, float *ty)
{
	ext_x = ext_x > 0.0f ? ext_x : 0.0f;
	ext_y = ext_y > 0.0f ? ext_y : 0.0f;
	float kx = w > 0.0f ? w / (w + 2.0f * ext_x) : 1.0f;
	float ky = h > 0.0f ? h / (h + 2.0f * ext_y) : 1.0f;
	*k = kx < ky ? kx : ky;
	*tx = (w - (w + 2.0f * ext_x) * *k) / 2.0f + ext_x * *k;
	*ty = (h - (h + 2.0f * ext_y) * *k) / 2.0f + ext_y * *k;
}
