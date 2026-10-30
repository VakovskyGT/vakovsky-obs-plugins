#include "frame-render.h"

#include <obs-module.h>
#include <util/platform.h>

#include <math.h>

#include <plugin-support.h>

enum vf_param_id {
	P_IMAGE,
	P_SIZE,
	P_RECT,
	P_PX_SCALE,
	P_RADII,
	P_PAD,
	P_IN_W,
	P_IN_SOFT,
	P_IN_C0,
	P_IN_C1,
	P_IN_DIR,
	P_IN_FILL,
	P_OUT_W,
	P_OUT_SOFT,
	P_OUT_C0,
	P_OUT_C1,
	P_OUT_DIR,
	P_OUT_FILL,
	P_SH_ON,
	P_SH_TYPE,
	P_SH_COL,
	P_SH_BLUR,
	P_SH_OFF,
	P_ZOOM,
	P_ZOOM_OFF,
	P_SRC_SIZE,
	P_CROP_BOX,
	P_BK_C0,
	P_BK_C1,
	P_BK_DIR,
	P_BK_FILL,
	P_EDGE_W,
	P_EDGE_EXT,
	P_COUNT
};

static const char *param_names[P_COUNT] = {
	"image", "size",    "rect",    "px_scale", "radii",    "pad",      "in_w",     "in_soft",  "in_c0",
	"in_c1", "in_dir",  "in_fill", "out_w",    "out_soft", "out_c0",   "out_c1",   "out_dir",  "out_fill",
	"sh_on", "sh_type", "sh_col",  "sh_blur",  "sh_off",   "zoom",     "zoom_off", "src_size", "crop_box",
	"bk_c0", "bk_c1",   "bk_dir",  "bk_fill",  "edge_w",   "edge_ext",
};

struct vf_render {
	gs_effect_t *effect;
	gs_eparam_t *params[P_COUNT];
	gs_technique_t *tech_basic, *tech_full;

	gs_texrender_t *capture;
	enum gs_color_format capture_format;
	uint64_t captured_frame;
	uint32_t captured_w, captured_h;         /* capture texture size */
	uint32_t captured_src_w, captured_src_h; /* source size it was captured from */
	uint64_t last_used_ns;
};

struct vf_render *vf_render_create(void)
{
	struct vf_render *r = bzalloc(sizeof(*r));
	char *path = obs_module_file("frame.effect");
	char *err = NULL;
	r->effect = path ? gs_effect_create_from_file(path, &err) : NULL;
	if (!r->effect)
		obs_log(LOG_ERROR, "failed to load %s: %s", path ? path : "frame.effect", err ? err : "unknown error");
	bfree(err);
	bfree(path);

	if (r->effect) {
		for (int i = 0; i < P_COUNT; i++)
			r->params[i] = gs_effect_get_param_by_name(r->effect, param_names[i]);
		r->tech_basic = gs_effect_get_technique(r->effect, "Basic");
		r->tech_full = gs_effect_get_technique(r->effect, "Full");
	}
	return r;
}

void vf_render_destroy(struct vf_render *r)
{
	if (!r)
		return;
	gs_texrender_destroy(r->capture);
	gs_effect_destroy(r->effect);
	bfree(r);
}

enum gs_color_space vf_render_color_space(obs_source_t *filter, size_t count, const enum gs_color_space *preferred)
{
	const enum gs_color_space potential[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	obs_source_t *target = obs_filter_get_target(filter);
	const enum gs_color_space source_space =
		target ? obs_source_get_color_space(target, OBS_COUNTOF(potential), potential) : GS_CS_SRGB;

	enum gs_color_space space = source_space;
	for (size_t i = 0; i < count; i++) {
		space = preferred[i];
		if (space == source_space)
			break;
	}
	return space;
}

static void set_vec2(gs_eparam_t *p, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(p, &v);
}

static void set_vec4(gs_eparam_t *p, float x, float y, float z, float w)
{
	struct vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(p, &v);
}

static void set_color(gs_eparam_t *p, struct vf_color c, float opacity)
{
	set_vec4(p, c.r, c.g, c.b, c.a * opacity);
}

static void set_fill(struct vf_render *r, int base, const struct vf_stroke *s)
{
	/* base is P_IN_C0, P_OUT_C0 or P_BK_C0; c0, c1, dir and fill follow in the same order */
	float a = s->angle_deg * 3.14159265f / 180.0f;
	set_color(r->params[base + 0], s->c0, s->opacity);
	set_color(r->params[base + 1], s->c1, s->opacity);
	set_vec2(r->params[base + 2], cosf(a), sinf(a));
	gs_effect_set_float(r->params[base + 3], s->fill == VF_FILL_LINEAR ? 1.0f : 0.0f);
}

static void set_stroke(struct vf_render *r, int base, const struct vf_stroke *s)
{
	/* base is P_IN_W or P_OUT_W; softness and the fill follow in the same order */
	gs_effect_set_float(r->params[base + 0], s->width);
	gs_effect_set_float(r->params[base + 1], s->softness);
	set_fill(r, base + 2, s);
}

/* Item space: the cropped part sits at [0, cw] x [0, ch]; qw x qh is the quad drawn for it (whole px). */
struct vf_geom {
	float src_w, src_h;   /* source under the filter */
	float ox, oy, cw, ch; /* cropped part in source px */
	uint32_t qw, qh;
	float clip_w, clip_h; /* OBS's clip of a texture-path item (the reported size), 0 = none */
};

static void set_uniforms(struct vf_render *r, const struct vf_geom *g, const struct vf_params *p, float sx, float sy,
			 const int crop[4], float pad)
{
	float radii[4];
	vf_params_radii(p, radii);

	set_vec2(r->params[P_SIZE], (float)g->qw, (float)g->qh);
	/* the frame shape: the cropped part, minus OBS's own (Alt) crop of the item */
	float shape[4];
	vf_shape_rect(g->cw, g->ch, crop, g->clip_w, g->clip_h, shape);
	set_vec4(r->params[P_RECT], shape[0], shape[1], shape[2], shape[3]);
	set_vec2(r->params[P_SRC_SIZE], g->src_w, g->src_h);
	set_vec4(r->params[P_CROP_BOX], g->ox, g->oy, g->ox + g->cw, g->oy + g->ch);
	set_vec2(r->params[P_PX_SCALE], sx, sy);
	set_vec4(r->params[P_RADII], radii[VF_TL], radii[VF_TR], radii[VF_BR], radii[VF_BL]);
	gs_effect_set_float(r->params[P_PAD], pad);

	set_stroke(r, P_IN_W, &p->inner);
	set_stroke(r, P_OUT_W, &p->outer);
	set_fill(r, P_BK_C0, &p->backing);

	gs_effect_set_float(r->params[P_SH_ON], p->shadow_on ? 1.0f : 0.0f);
	gs_effect_set_float(r->params[P_SH_TYPE], p->shadow_type == VF_SHADOW_INNER ? 1.0f : 0.0f);
	set_color(r->params[P_SH_COL], p->shadow_color, p->shadow_opacity);
	gs_effect_set_float(r->params[P_SH_BLUR], p->shadow_blur);
	set_vec2(r->params[P_SH_OFF], p->shadow_x, p->shadow_y);

	gs_effect_set_float(r->params[P_ZOOM], p->zoom);
	set_vec2(r->params[P_ZOOM_OFF], p->content_x, p->content_y);

	float ew[4] = {0.0f, 0.0f, 0.0f, 0.0f}, ext[4];
	if (p->edge_on)
		vf_params_edge_widths(p, ew);
	vf_params_outer_extents(p, ext);
	set_vec4(r->params[P_EDGE_W], ew[VF_CROP_L], ew[VF_CROP_T], ew[VF_CROP_R], ew[VF_CROP_B]);
	set_vec4(r->params[P_EDGE_EXT], ext[VF_CROP_L], ext[VF_CROP_T], ext[VF_CROP_R], ext[VF_CROP_B]);
}

/* Renders the rest of the filter chain into our texture, at most once per video frame. */
/* Captures at cw x ch (the size the source is actually seen at, see vf_capture_scale); the source is still drawn
 * in its own w x h coordinates, so the shader keeps working in source pixels. A later render in the same frame that
 * needs a larger capture (another copy shown bigger) re-captures. */
static gs_texture_t *capture_source(struct vf_render *r, obs_source_t *filter, uint32_t w, uint32_t h, uint32_t cw,
				    uint32_t ch, enum gs_color_space space)
{
	const enum gs_color_format format = gs_get_format_from_space(space);
	const uint64_t frame = obs_get_video_frame_time();

	if (r->capture && r->capture_format != format) {
		gs_texrender_destroy(r->capture);
		r->capture = NULL;
	}
	if (!r->capture) {
		r->capture = gs_texrender_create(format, GS_ZS_NONE);
		r->capture_format = format;
		r->captured_frame = 0;
	}

	bool fresh = r->captured_frame == frame && r->captured_src_w == w && r->captured_src_h == h;
	if (!fresh || cw > r->captured_w || ch > r->captured_h) {
		gs_texrender_reset(r->capture);
		if (gs_texrender_begin_with_color_space(r->capture, cw, ch, space)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_blend_state_push();
			gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
						   GS_BLEND_INVSRCALPHA);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			obs_source_skip_video_filter(filter);
			gs_blend_state_pop();
			gs_texrender_end(r->capture);
		}
		r->captured_frame = frame;
		r->captured_w = cw;
		r->captured_h = ch;
		r->captured_src_w = w;
		r->captured_src_h = h;
	}
	return gs_texrender_get_texture(r->capture);
}

void vf_render_draw(struct vf_render *r, obs_source_t *filter, uint32_t w, uint32_t h, const struct vf_params *p,
		    float sx, float sy, const int crop[4], uint32_t clip_w, uint32_t clip_h)
{
	if (!r->effect || !w || !h || vf_params_is_noop(p)) {
		obs_source_skip_video_filter(filter);
		return;
	}

	struct vf_geom g = {(float)w, (float)h};
	vf_params_crop_rect(p, g.src_w, g.src_h, &g.ox, &g.oy, &g.cw, &g.ch);
	g.qw = (uint32_t)ceilf(g.cw);
	g.qh = (uint32_t)ceilf(g.ch);
	g.clip_w = (float)clip_w;
	g.clip_h = (float)clip_h;

	const enum gs_color_space preferred[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	const enum gs_color_space space = vf_render_color_space(filter, OBS_COUNTOF(preferred), preferred);
	const char *tech_name = vf_params_needs_full(p) ? "Full" : "Basic";
	const float pad = vf_pad_source_px(vf_params_outer_extent(p), sx, sy);

	if (pad <= 0.0f) {
		/* Fast path: nothing beyond the edges, the quad of OBS's own filter pass is enough. */
		if (!obs_source_process_filter_begin_with_color_space(filter, gs_get_format_from_space(space), space,
								      OBS_NO_DIRECT_RENDERING))
			return;
		set_uniforms(r, &g, p, sx, sy, crop, 0.0f);
		obs_source_process_filter_tech_end(filter, r->effect, g.qw, g.qh, tech_name);
		return;
	}

	const float cs = vf_capture_scale(sx, sy, p->zoom);
	uint32_t cw = (uint32_t)ceilf((float)w * cs), ch = (uint32_t)ceilf((float)h * cs);
	gs_texture_t *tex = capture_source(r, filter, w, h, cw ? cw : 1, ch ? ch : 1, space);
	r->last_used_ns = os_gettime_ns();
	if (!tex)
		return;

	set_uniforms(r, &g, p, sx, sy, crop, pad);

	const bool prev_linear = gs_set_linear_srgb(true);
	const bool prev_fb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(r->params[P_IMAGE], tex);

	const uint32_t qw = g.qw + (uint32_t)(2.0f * pad), qh = g.qh + (uint32_t)(2.0f * pad);
	gs_matrix_push();
	gs_matrix_translate3f(-pad, -pad, 0.0f);
	gs_technique_t *tech = vf_params_needs_full(p) ? r->tech_full : r->tech_basic;
	size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(tex, 0, qw, qh);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
	gs_matrix_pop();

	gs_enable_framebuffer_srgb(prev_fb);
	gs_set_linear_srgb(prev_linear);
}

void vf_render_release_idle(struct vf_render *r, uint64_t now_ns, uint64_t idle_ns)
{
	if (r && r->capture && now_ns - r->last_used_ns > idle_ns) {
		gs_texrender_destroy(r->capture);
		r->capture = NULL;
	}
}
