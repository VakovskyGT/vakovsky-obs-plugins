/* GPU side of Ultimate Frame: source capture (once per frame), shader technique choice, drawing beyond the edges. */
#pragma once

#include <obs.h>

#include "vf-params.h"

struct vf_render;

/* Must be called inside the graphics context. */
struct vf_render *vf_render_create(void);
void vf_render_destroy(struct vf_render *r);

/* crop: left, top, right, bottom of the visible rect in source px. */
void vf_render_draw(struct vf_render *r, obs_source_t *filter, uint32_t w, uint32_t h, const struct vf_params *p,
		    float sx, float sy, const int crop[4], uint32_t clip_w, uint32_t clip_h);

/* Frees the capture texture if it has not been used for idle_ns. Graphics thread. */
void vf_render_release_idle(struct vf_render *r, uint64_t now_ns, uint64_t idle_ns);

/* Color space the filter outputs for its target. */
enum gs_color_space vf_render_color_space(obs_source_t *filter, size_t count, const enum gs_color_space *preferred);
