/* Fade for Ultimate Frame transition: pure layout of the margin texture. */
#pragma once

#include <stdint.h>

/* Children are drawn into a texture that extends a margin beyond the transition on every side, so outer effects
 * drawn past a child's edge survive the fade. The texture is rendered at canvas resolution: a filter inside sees
 * the canvas scale of the item it belongs to, as it does when drawn directly. */
struct vf_fade_layout {
	float mx, my;          /* margin in transition px */
	float quad_w, quad_h;  /* drawn quad, transition px: the whole texture (size + 2 * margin, rounded up) */
	uint32_t tex_w, tex_h; /* texture size, px: quad at canvas scale */
	float sx, sy;          /* child scale inside the transition */
	float ox, oy;          /* child offset inside the transition (before the margin), px */
};

#define VF_FADE_MAX_TEX 8192u

/* margin is in canvas px; canvas_sx/sy are canvas px per transition px (the render matrix scale). Children are
 * aspect-fit and centered, as OBS_TRANSITION_SCALE_ASPECT with center alignment; equal sizes give identity. */
void vf_fade_layout(uint32_t child_w, uint32_t child_h, uint32_t tr_w, uint32_t tr_h, float margin, float canvas_sx,
		    float canvas_sy, struct vf_fade_layout *out);

/* Graphics render scope; nested scene renders restore their caller's scope. Other threads see NULL. */
const char *vf_fade_scene_push(const char *scene_uuid);
const char *vf_fade_scene_get(void);
