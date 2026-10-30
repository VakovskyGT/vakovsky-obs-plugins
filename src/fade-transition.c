/* Fade for Ultimate Frame: a cross-fade that does not clip what children draw past their edge (Ultimate Frame's outer effects). */
#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <math.h>
#include <string.h>

#include "vf-about.h"
#include "vf-fade.h"

#define S_MARGIN "margin"

struct vf_fade {
	obs_source_t *src;
	gs_effect_t *effect;
	gs_eparam_t *p_a, *p_b, *p_fade;
	gs_texrender_t *tex[2];
	float margin;
};

static const char *vf_fade_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FadeForFrame");
}

static void vf_fade_update(void *data, obs_data_t *settings)
{
	struct vf_fade *f = data;
	f->margin = (float)obs_data_get_int(settings, S_MARGIN);
}

static void *vf_fade_create(obs_data_t *settings, obs_source_t *src)
{
	struct vf_fade *f = bzalloc(sizeof(*f));
	f->src = src;

	char *path = obs_module_file("fade.effect");
	obs_enter_graphics();
	char *err = NULL;
	f->effect = path ? gs_effect_create_from_file(path, &err) : NULL;
	if (f->effect) {
		f->p_a = gs_effect_get_param_by_name(f->effect, "tex_a");
		f->p_b = gs_effect_get_param_by_name(f->effect, "tex_b");
		f->p_fade = gs_effect_get_param_by_name(f->effect, "fade_val");
	}
	f->tex[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->tex[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	if (!f->effect)
		obs_log(LOG_ERROR, "fade.effect failed: %s", err ? err : "not found");
	bfree(err);
	bfree(path);

	vf_fade_update(f, settings);
	return f;
}

static void vf_fade_destroy(void *data)
{
	struct vf_fade *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->tex[0]);
	gs_texrender_destroy(f->tex[1]);
	obs_leave_graphics();
	bfree(f);
}

static void render_child(gs_texrender_t *tr, obs_source_t *child, const struct vf_fade_layout *l, float csx, float csy)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin_with_color_space(tr, l->tex_w, l->tex_h, GS_CS_SRGB))
		return;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	if (child) {
		/* canvas resolution: a filter inside sees its item's canvas scale, as when drawn directly. The margin goes
		 * into the projection, not the matrix: at canvas scale 1 children get the same matrix as with OBS's fade,
		 * so Ultimate Frame still identifies its copies by their draw transform. */
		gs_ortho(-l->mx * csx, (l->quad_w - l->mx) * csx, -l->my * csy, (l->quad_h - l->my) * csy, -100.0f,
			 100.0f);
		gs_matrix_push();
		gs_matrix_identity();
		gs_matrix_scale3f(csx, csy, 1.0f);
		gs_matrix_translate3f(l->ox, l->oy, 0.0f);
		gs_matrix_scale3f(l->sx, l->sy, 1.0f);
		gs_blend_state_push();
		gs_reset_blend_state();
		obs_source_video_render(child);
		gs_blend_state_pop();
		gs_matrix_pop();
	}
	gs_texrender_end(tr);
}

static void vf_fade_video_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct vf_fade *f = data;
	obs_source_t *transition = f->src;
	float t = obs_transition_get_time(transition);

	/* t is 1 when idle and at the end (obs_transition_is_active does not exist in OBS 31) */
	if (!f->effect || t >= 1.0f) {
		/* Not fading: let libobs draw the current child and stop the transition at its end. */
		obs_transition_video_render_direct(transition, OBS_TRANSITION_SOURCE_B);
		return;
	}

	uint32_t cx = 0, cy = 0;
	obs_transition_get_size(transition, &cx, &cy);
	/* Zero means automatic in obs_transition_get_size; the base size resolves it from the children. */
	if (!cx)
		cx = obs_source_get_base_width(transition);
	if (!cy)
		cy = obs_source_get_base_height(transition);
	if (!cx || !cy)
		return;

	obs_source_t *child[2] = {obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_A),
				  obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B)};
	struct matrix4 cur;
	gs_matrix_get(&cur);
	float csx = sqrtf(cur.x.x * cur.x.x + cur.x.y * cur.x.y + cur.x.z * cur.x.z);
	float csy = sqrtf(cur.y.x * cur.y.x + cur.y.y * cur.y.y + cur.y.z * cur.y.z);
	if (!(csx > 1e-4f) || !(csy > 1e-4f))
		csx = csy = 1.0f;

	struct vf_fade_layout l = {0};
	for (int i = 0; i < 2; i++) {
		uint32_t w = child[i] ? obs_source_get_width(child[i]) : 0;
		uint32_t h = child[i] ? obs_source_get_height(child[i]) : 0;
		vf_fade_layout(w, h, cx, cy, f->margin, csx, csy, &l);
		render_child(f->tex[i], child[i], &l, csx, csy);
	}

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_set_texture_srgb(f->p_a, gs_texrender_get_texture(f->tex[0]));
	gs_effect_set_texture_srgb(f->p_b, gs_texrender_get_texture(f->tex[1]));
	gs_effect_set_float(f->p_fade, t);

	gs_matrix_push();
	gs_matrix_translate3f(-l.mx, -l.my, 0.0f);
	gs_matrix_scale3f(l.quad_w / (float)l.tex_w, l.quad_h / (float)l.tex_h, 1.0f);
	while (gs_effect_loop(f->effect, "Fade"))
		gs_draw_sprite(NULL, 0, l.tex_w, l.tex_h);
	gs_matrix_pop();

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);

	obs_source_release(child[0]);
	obs_source_release(child[1]);
}

static float mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

static bool vf_fade_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
				 size_t channels, size_t sample_rate)
{
	struct vf_fade *f = data;
	return obs_transition_audio_render(f->src, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
}

static obs_properties_t *vf_fade_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *margin = obs_properties_add_int(props, S_MARGIN, obs_module_text("Fade.Margin"), 0, 1000, 10);
	obs_property_int_set_suffix(margin, " px");
	obs_property_set_long_description(margin, obs_module_text("Fade.MarginTip"));
	vf_add_about(props, "FadeForFrame");
	return props;
}

static void vf_fade_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_MARGIN, 200);
}

struct obs_source_info vf_fade_transition_info = {
	.id = "vakovsky_fade_for_ultimate_frame",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = vf_fade_get_name,
	.create = vf_fade_create,
	.destroy = vf_fade_destroy,
	.update = vf_fade_update,
	.video_render = vf_fade_video_render,
	.audio_render = vf_fade_audio_render,
	.get_properties = vf_fade_properties,
	.get_defaults = vf_fade_defaults,
};
