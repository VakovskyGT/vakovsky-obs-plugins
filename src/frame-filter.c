/* Ultimate Frame filter: settings, properties and per-render copy identification. */
#include <obs-module.h>
#include <graphics/matrix4.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

#include "vf-about.h"
#include "copy-registry.h"
#include "frame-render.h"
#include "vf-fade.h"
#include "vf-match.h"
#include "vf-preview.h"
#include "vf-params.h"
#include "profile-prune.h"
#include "vf-store.h"
#include "vf-select.h"
#include "scene-anim.h"
#include <util/platform.h>

#define VF_MAX_COPIES 64

struct vf_filter {
	obs_source_t *self;
	struct vf_render *render;

	pthread_mutex_t lock; /* guards store between update and render */
	struct vf_store store;

	/* OBS defers update of video sources to the graphics thread: it only reads settings, and this lock keeps
	 * those reads off our own writes from the UI thread (editor callbacks). */
	pthread_mutex_t settings_lock;

	/* UI thread only */
	char edit_owner[VF_UUID_LEN]; /* profile the flat editor fields belong to ("" = default) */
	uint64_t last_editor_ns;      /* last get_properties / editor callback, to tell a new editor session */

	/* render thread only (under lock for simplicity) */
	struct vf_anim_run run;
	struct vf_params last_program; /* what was last shown for the program scene */
	bool have_last_program;
	char view_scene[VF_UUID_LEN]; /* edit_owner, for the filters dialog's preview */
	volatile long editors;        /* properties alive: the filters dialog shows this filter */

	/* size reported to OBS (the program's cropped part), computed once per frame in video_tick; 0 = not yet */
	volatile long size_w, size_h;
};

/* All live filters, for the periodic idle-texture release. */
static pthread_mutex_t instances_mutex = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(struct vf_filter *) instances;

#define VF_IDLE_NS 10000000000ULL

void vf_filters_release_idle(uint64_t now_ns)
{
	pthread_mutex_lock(&instances_mutex);
	obs_enter_graphics();
	for (size_t i = 0; i < instances.num; i++)
		vf_render_release_idle(instances.array[i]->render, now_ns, VF_IDLE_NS);
	obs_leave_graphics();
	pthread_mutex_unlock(&instances_mutex);
}

static const char *vf_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("UltimateFrame");
}

static const char *edit_key(obs_data_t *settings)
{
	return obs_data_get_bool(settings, "per_scene") ? obs_data_get_string(settings, "edit_scene") : "";
}

static void vf_update(void *data, obs_data_t *settings)
{
	struct vf_filter *f = data;

	struct vf_store st, old;
	pthread_mutex_lock(&f->settings_lock);
	vf_store_load(settings, &st);
	pthread_mutex_unlock(&f->settings_lock);
	pthread_mutex_lock(&f->lock);
	old = f->store;
	f->store = st;
	pthread_mutex_unlock(&f->lock);
	vf_store_free(&old);
}

static void *vf_create(obs_data_t *settings, obs_source_t *source)
{
	struct vf_filter *f = bzalloc(sizeof(*f));
	f->self = source;
	pthread_mutex_init(&f->lock, NULL);
	pthread_mutex_init(&f->settings_lock, NULL);
	snprintf(f->edit_owner, sizeof(f->edit_owner), "%s", edit_key(settings));
	snprintf(f->view_scene, sizeof(f->view_scene), "%s", f->edit_owner);

	obs_enter_graphics();
	f->render = vf_render_create();
	obs_leave_graphics();

	vf_update(f, settings);

	pthread_mutex_lock(&instances_mutex);
	da_push_back(instances, &f);
	pthread_mutex_unlock(&instances_mutex);
	return f;
}

static void vf_destroy(void *data)
{
	struct vf_filter *f = data;
	pthread_mutex_lock(&instances_mutex);
	da_erase_item(instances, &f);
	if (!instances.num)
		da_free(instances); /* the last filter: nothing may be left allocated at module unload */
	pthread_mutex_unlock(&instances_mutex);

	obs_enter_graphics();
	vf_render_destroy(f->render);
	obs_leave_graphics();
	pthread_mutex_destroy(&f->lock);
	pthread_mutex_destroy(&f->settings_lock);
	vf_store_free(&f->store);
	bfree(f);
}

static void vf_defaults(obs_data_t *settings)
{
	vf_params_set_defaults(settings);
	vf_store_set_defaults(settings);
}

/* ---------- render ---------- */

static float axis_len(const struct vec4 *v)
{
	return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
}

/* A scene used as a source (not a group): OBS and Move always draw it through a texture of its own size, so nothing
 * past its edge can show. */
static bool parent_is_scene(obs_source_t *parent)
{
	return parent && obs_source_get_type(parent) == OBS_SOURCE_TYPE_SCENE && !obs_source_is_group(parent);
}

/* The first copy shown in the scene, or -1. */
static int scene_copy(const struct vf_reg_copy *rc, size_t n, const char *uuid)
{
	for (size_t i = 0; uuid[0] && i < n; i++)
		if (strcmp(rc[i].scene_uuid, uuid) == 0 && vf_reg_copy_shown(&rc[i]))
			return (int)i;
	return -1;
}

static bool has_scene(const struct vf_reg_copy *rc, size_t n, const char *uuid)
{
	for (size_t i = 0; uuid[0] && i < n; i++)
		if (strcmp(rc[i].scene_uuid, uuid) == 0 && vf_reg_copy_shown(&rc[i]))
			return true;
	return false;
}

/* The OBS transition of the last program switch is still running (it can outlast our animation). */
static bool transition_active(const struct vf_anim_state *as, uint64_t now)
{
	return as->start_ns && as->transition_ms && now - as->start_ns < (uint64_t)as->transition_ms * 1000000ULL;
}

/* Eased progress of the running scene-switch animation, or -1. */
static float anim_progress(const struct vf_store *st, const struct vf_anim_state *as, uint64_t now)
{
	if (!st->per_scene || !as->start_ns)
		return -1.0f;
	return vf_anim_progress(st->anim_on, as->transition_ms, st->anim_ms, as->move_easing, as->move_easing_fn,
				(double)(now - as->start_ns) / 1e6);
}

/* The profile values shown for a selection, advancing the shared animation run. Caller holds f->lock. */
static void shown_params(struct vf_filter *f, const struct vf_anim_state *as, struct vf_select_out sel, float t,
			 struct vf_params *p)
{
	const struct vf_params *target_params = vf_store_find(&f->store, sel.kind == VF_SEL_DEFAULT ? NULL : sel.scene);
	if (sel.kind == VF_SEL_LERP) {
		const struct vf_params *start = f->have_last_program ? &f->last_program
								     : vf_store_find(&f->store, as->from_uuid);
		vf_anim_run_sync(&f->run, as->start_ns, start);
		vf_anim_run_value(&f->run, target_params, t, p);
	} else {
		*p = *target_params;
	}
}

/* Once per frame, before rendering: the size OBS sees is the cropped part of what the program shows. */
static void vf_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct vf_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->self);
	obs_source_t *parent = obs_filter_get_parent(f->self);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!parent || !w || !h) {
		os_atomic_set_long(&f->size_w, 0);
		os_atomic_set_long(&f->size_h, 0);
		return;
	}

	struct vf_anim_state as;
	vf_anim_get(&as);
	/* the UI thread may release the registry (collection change, exit) while this tick runs: read it locked */
	bool in_from, in_program;
	vf_registry_in_scenes(parent, as.from_uuid, as.program_uuid, &in_from, &in_program);

	struct vf_params p;
	pthread_mutex_lock(&f->lock);
	float t = anim_progress(&f->store, &as, os_gettime_ns());
	if (t >= 0.0f && !in_from)
		t = -1.0f;
	struct vf_select_in in = {
		.per_scene = f->store.per_scene,
		.program_scene = as.program_uuid,
		.anim_from = as.from_uuid,
		.anim_to = as.program_uuid,
		.anim_t = t,
		.source_in_program = in_program,
		.transition_active = transition_active(&as, os_gettime_ns()),
	};
	struct vf_select_out sel = vf_select_program(&in);
	shown_params(f, &as, sel, t, &p);
	pthread_mutex_unlock(&f->lock);

	float ox, oy, cw, ch;
	vf_params_crop_rect(&p, (float)w, (float)h, &ox, &oy, &cw, &ch);
	os_atomic_set_long(&f->size_w, lroundf(cw) > 0 ? lroundf(cw) : 1);
	os_atomic_set_long(&f->size_h, lroundf(ch) > 0 ? lroundf(ch) : 1);
}

static uint32_t reported_size(struct vf_filter *f, bool height)
{
	long v = os_atomic_load_long(height ? &f->size_h : &f->size_w);
	if (v > 0)
		return (uint32_t)v;
	obs_source_t *target = obs_filter_get_target(f->self);
	return target ? (height ? obs_source_get_base_height(target) : obs_source_get_base_width(target)) : 0;
}

static uint32_t vf_get_width(void *data)
{
	return reported_size(data, false);
}

static uint32_t vf_get_height(void *data)
{
	return reported_size(data, true);
}

/* The copy the dialog preview stands for: the edited scene's, else the program scene's; -1 when the source is in neither */
static int preview_copy(const struct vf_reg_copy *rc, size_t n, const char *view, const char *program)
{
	for (int pass = 0; pass < 2; pass++) {
		const char *want = pass ? program : view;
		for (size_t i = 0; want && *want && i < n; i++)
			if (strcmp(rc[i].scene_uuid, want) == 0)
				return (int)i;
	}
	return -1;
}

static void vf_video_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct vf_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->self);
	obs_source_t *parent = obs_filter_get_parent(f->self);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;

	if (!parent || !w || !h) {
		obs_source_skip_video_filter(f->self);
		return;
	}

	struct matrix4 m;
	gs_matrix_get(&m);

	struct vf_anim_state as;
	vf_anim_get(&as);

	const struct vf_reg_copy *rc = NULL;
	size_t n = vf_registry_get(parent, &rc);
	if (n > VF_MAX_COPIES)
		n = VF_MAX_COPIES;

	/* A window (Studio Mode preview, multiview, projector) draws straight into its swapchain: no texture target.
	 * The output and any nested texture render (a transition of Move fading an item, an item texture) have one. */
	const bool window_render = gs_get_render_target() == NULL;

	/* items (draw_transform, Alt crop) are laid out by the size we report, not the size under the filter */
	const uint32_t rw = reported_size(f, false), rh = reported_size(f, true);
	struct vf_copy copies[VF_MAX_COPIES];
	const char *scope = vf_fade_scene_get(); /* the scene a transition is drawing, if any */
	for (size_t i = 0; i < n; i++) {
		int rank =
			vf_copy_rank(rc[i].scene_uuid, as.program_uuid, as.from_uuid, as.preview_uuid, window_render);
		rank = vf_copy_rank_scoped(rank, rc[i].scene_uuid, scope);
		vf_reg_copy_fill(&rc[i], rw, rh, rank, &copies[i]);
	}

	struct vf_match_result mr = vf_match_copy((const float *)&m, rw, rh, copies, n, window_render);

	/* The filters dialog (or a source projector): show the copy of the edited scene as if it were drawn there */
	struct gs_rect vp;
	gs_get_viewport(&vp);
	struct obs_video_info ovi = {0};
	if (mr.kind == VF_MATCH_EXACT) /* only an exact match needs the canvas */
		obs_get_video_info(&ovi);
	struct vf_preview_in pin = {
		.match = mr.kind,
		.editor_open = os_atomic_load_long(&f->editors) > 0,
		.view_w = vp.cx,
		.view_h = vp.cy,
		.src_w = rw,
		.src_h = rh,
		.canvas_w = ovi.base_width,
		.canvas_h = ovi.base_height,
	};
	memcpy(pin.m, &m, sizeof(pin.m));
	const bool source_view = vf_preview_is_source_view(&pin);
	if (source_view) {
		char view[VF_UUID_LEN];
		pthread_mutex_lock(&f->lock);
		snprintf(view, sizeof(view), "%s", f->view_scene);
		pthread_mutex_unlock(&f->lock);
		mr.index = preview_copy(rc, n, view, as.program_uuid);
	}

	struct vf_params p;
	pthread_mutex_lock(&f->lock);
	float t = anim_progress(&f->store, &as, os_gettime_ns());
	if (t >= 0.0f && !has_scene(rc, n, as.from_uuid))
		t = -1.0f; /* the source was not in the previous scene: nothing to animate from */

	struct vf_select_in in = {
		.per_scene = f->store.per_scene,
		.matched = mr.index >= 0,
		.copy_scene = mr.index >= 0 ? rc[mr.index].scene_uuid : NULL,
		.program_scene = as.program_uuid,
		.anim_from = as.from_uuid,
		.anim_to = as.program_uuid,
		.anim_t = t,
		.source_in_program = has_scene(rc, n, as.program_uuid),
		.transition_active = transition_active(&as, os_gettime_ns()),
	};
	struct vf_select_out sel = vf_select(&in);
	shown_params(f, &as, sel, t, &p);
	/* the dialog preview only looks: what the program showed last is where the next switch starts from */
	if (!source_view &&
	    (sel.kind == VF_SEL_LERP || (sel.kind == VF_SEL_SCENE && strcmp(sel.scene, as.program_uuid) == 0))) {
		f->last_program = p;
		f->have_last_program = true;
	}
	pthread_mutex_unlock(&f->lock);

	float sx, sy;
	int crop[4] = {0, 0, 0, 0};
	if (mr.index >= 0) {
		const struct vf_copy *c = &copies[mr.index];
		vf_copy_scale(c, &sx, &sy);
		/* Move draws a nested scene through its own texture at no scale: follow it from scene to scene along
		 * Move's curve, whether or not the profiles animate */
		float move_t = c->texpath && !window_render && !scope && as.start_ns
				       ? vf_anim_progress(true, as.transition_ms, 0, as.move_easing, as.move_easing_fn,
							  (double)(os_gettime_ns() - as.start_ns) / 1e6)
				       : -1.0f;
		if (move_t >= 0.0f) {
			int from = scene_copy(rc, n, as.from_uuid), to = scene_copy(rc, n, as.program_uuid);
			if (from >= 0 || to >= 0)
				vf_copy_scale_moving(from >= 0 ? &copies[from] : NULL, to >= 0 ? &copies[to] : NULL,
						     move_t, &sx, &sy);
		}
		if (c->texpath) {
			/* OBS clips this item to its visible rect: anything beyond the edge would show as fragments */
			vf_params_strip_outer(&p);
			crop[0] = c->crop_l;
			crop[1] = c->crop_t;
			crop[2] = c->crop_r;
			crop[3] = c->crop_b;
		}
	} else {
		sx = axis_len(&m.x);
		sy = axis_len(&m.y);
	}
	if (sx <= 1e-4f || sy <= 1e-4f)
		sx = sy = 1.0f;
	if (parent_is_scene(parent))
		vf_params_strip_outer(&p); /* the outer effects are off on a scene, in every render */

	/* OBS clips a texture-path item at the size we report */
	const bool texpath = mr.index >= 0 && copies[mr.index].texpath;
	if (source_view) {
		/* outer effects reach past the source: shrink the whole frame into the window, centred */
		float ext = vf_params_outer_extent(&p), k, tx, ty;
		vf_preview_fit((float)rw, (float)rh, ext / sx, ext / sy, &k, &tx, &ty);
		gs_matrix_push();
		gs_matrix_translate3f(tx, ty, 0.0f);
		gs_matrix_scale3f(k, k, 1.0f);
		vf_render_draw(f->render, f->self, w, h, &p, sx, sy, crop, texpath ? rw : 0, texpath ? rh : 0);
		gs_matrix_pop();
		return;
	}
	vf_render_draw(f->render, f->self, w, h, &p, sx, sy, crop, texpath ? rw : 0, texpath ? rh : 0);
}

static enum gs_color_space vf_get_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	struct vf_filter *f = data;
	return vf_render_color_space(f->self, count, preferred);
}

/* ---------- properties ---------- */

static void stroke_visibility(obs_properties_t *props, obs_data_t *settings, const char *pre)
{
	char k[32];
	snprintf(k, sizeof(k), "%s_on", pre);
	bool on = obs_data_get_bool(settings, k);
	snprintf(k, sizeof(k), "%s_fill", pre);
	bool linear = obs_data_get_int(settings, k) == VF_FILL_LINEAR;
	/* the backing has no width or softness: obs_properties_get finds nothing and the call does nothing */
	static const char *const plain[] = {"w", "soft", "opacity", "fill", "c0"};
	for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
		snprintf(k, sizeof(k), "%s_%s", pre, plain[i]);
		obs_property_set_visible(obs_properties_get(props, k), on);
	}
	snprintf(k, sizeof(k), "%s_c1", pre);
	obs_property_set_visible(obs_properties_get(props, k), on && linear);
	snprintf(k, sizeof(k), "%s_angle", pre);
	obs_property_set_visible(obs_properties_get(props, k), on && linear);
}

/* A switched-off block shows only its checkbox and its "apply to all scenes" button (which copies the switch too). */
static bool blocks_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	bool corners = obs_data_get_bool(settings, "co_on");
	bool per = obs_data_get_bool(settings, "per_corner");
	obs_property_set_visible(obs_properties_get(props, "per_corner"), corners);
	obs_property_set_visible(obs_properties_get(props, "radius"), corners && !per);
	static const char *const corner_keys[4] = {"r_tl", "r_tr", "r_br", "r_bl"};
	for (int i = 0; i < 4; i++)
		obs_property_set_visible(obs_properties_get(props, corner_keys[i]), corners && per);

	stroke_visibility(props, settings, "in");
	stroke_visibility(props, settings, "out");
	stroke_visibility(props, settings, "bk");

	bool crop = obs_data_get_bool(settings, "crop_on");
	static const char *const crop_keys[4] = {"crop_l", "crop_t", "crop_r", "crop_b"};
	for (int i = 0; i < 4; i++)
		obs_property_set_visible(obs_properties_get(props, crop_keys[i]), crop);

	bool edge = obs_data_get_bool(settings, "ed_on");
	bool per_side = obs_data_get_bool(settings, "ed_per_side");
	obs_property_set_visible(obs_properties_get(props, "ed_per_side"), edge);
	obs_property_set_visible(obs_properties_get(props, "ed_w"), edge && !per_side);
	static const char *const edge_keys[4] = {"ed_l", "ed_t", "ed_r", "ed_b"};
	for (int i = 0; i < 4; i++)
		obs_property_set_visible(obs_properties_get(props, edge_keys[i]), edge && per_side);

	bool content = obs_data_get_bool(settings, "ct_on");
	static const char *const content_keys[3] = {"zoom", "cx", "cy"};
	for (int i = 0; i < 3; i++)
		obs_property_set_visible(obs_properties_get(props, content_keys[i]), content);
	return true;
}

static void add_block_switch(obs_properties_t *g, const char *key, const char *text_key)
{
	obs_property_t *on = obs_properties_add_bool(g, key, obs_module_text(text_key));
	obs_property_set_modified_callback(on, blocks_changed);
}

static bool anim_on_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	obs_property_set_visible(obs_properties_get(props, "anim_ms"), obs_data_get_bool(settings, "anim_on"));
	return true;
}

static bool shadow_on_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	/* a plain checkbox, not a checkable group: the group's "apply to all scenes" must work with the shadow off */
	bool on = obs_data_get_bool(settings, "sh_on");
	static const char *keys[] = {"sh_type", "sh_color", "sh_opacity", "sh_blur", "sh_x", "sh_y"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		obs_property_set_visible(obs_properties_get(props, keys[i]), on);
	return true;
}

/* Units go into the field (short labels line up); the values stay doubles in the settings, the step is a whole unit. */
static obs_property_t *add_unit(obs_properties_t *g, const char *key, const char *label, int min, int max,
				const char *suffix, const char *tip)
{
	obs_property_t *p = obs_properties_add_int_slider(g, key, obs_module_text(label), min, max, 1);
	obs_property_int_set_suffix(p, suffix);
	if (tip)
		obs_property_set_long_description(p, obs_module_text(tip));
	return p;
}

static obs_property_t *add_px(obs_properties_t *g, const char *key, const char *label, int min, int max)
{
	return add_unit(g, key, label, min, max, " px", "Unit.CanvasPxTip");
}

static obs_property_t *add_percent(obs_properties_t *g, const char *key, const char *label, int min, int max)
{
	return add_unit(g, key, label, min, max, " %", NULL);
}

/* "Apply to all scenes" buttons, one per block; shown with per-scene profiles only. */
static const char *const apply_buttons[] = {"crop_apply", "corners_apply", "in_apply",   "out_apply",
					    "bk_apply",   "sh_apply",      "edge_apply", "content_apply"};
static void add_apply_button(obs_properties_t *g, const char *name, obs_property_clicked_t cb, struct vf_filter *f);

/* A stroke block, or the backing (sized = false: a fill without width and softness). */
static void add_stroke_group(obs_properties_t *props, const char *pre, const char *title_key, bool sized,
			     obs_property_clicked_t apply_cb, struct vf_filter *f)
{
	char k[32], text[32];
	obs_properties_t *g = obs_properties_create();
	snprintf(k, sizeof(k), "%s_on", pre);
	snprintf(text, sizeof(text), "%s.On", title_key);
	add_block_switch(g, k, text);
	if (!sized)
		obs_property_set_long_description(obs_properties_get(g, k), obs_module_text("Backing.Tip"));

	if (sized) {
		snprintf(k, sizeof(k), "%s_w", pre);
		add_px(g, k, "Stroke.Width", 0, 200);
		snprintf(k, sizeof(k), "%s_soft", pre);
		add_px(g, k, "Stroke.Softness", 0, 100);
	}
	snprintf(k, sizeof(k), "%s_opacity", pre);
	add_percent(g, k, "Opacity", 0, 100);

	snprintf(k, sizeof(k), "%s_fill", pre);
	obs_property_t *fill = obs_properties_add_list(g, k, obs_module_text("Stroke.Fill"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fill, obs_module_text("Stroke.Fill.Solid"), VF_FILL_SOLID);
	obs_property_list_add_int(fill, obs_module_text("Stroke.Fill.Linear"), VF_FILL_LINEAR);
	obs_property_set_modified_callback(fill, blocks_changed);

	snprintf(k, sizeof(k), "%s_c0", pre);
	obs_properties_add_color_alpha(g, k, obs_module_text("Stroke.Color"));
	snprintf(k, sizeof(k), "%s_c1", pre);
	obs_properties_add_color_alpha(g, k, obs_module_text("Stroke.Color2"));
	snprintf(k, sizeof(k), "%s_angle", pre);
	add_unit(g, k, "Stroke.Angle", 0, 360, "°", NULL);
	snprintf(k, sizeof(k), "%s_apply", pre);
	add_apply_button(g, k, apply_cb, f);

	snprintf(k, sizeof(k), "%s_group", pre);
	obs_properties_add_group(props, k, obs_module_text(title_key), OBS_GROUP_NORMAL, g);
}

/* ---------- profiles & warnings ---------- */

/* UI thread. The flat fields still hold the profile they were loaded from (edit_owner): save them there, then load
 * the profile that edit_scene / per_scene now point at. */
/* UI thread */
static void set_edit_owner(struct vf_filter *f, const char *key)
{
	snprintf(f->edit_owner, sizeof(f->edit_owner), "%s", key);
	pthread_mutex_lock(&f->lock);
	snprintf(f->view_scene, sizeof(f->view_scene), "%s", key);
	pthread_mutex_unlock(&f->lock);
}

static void switch_edit_profile(struct vf_filter *f, obs_data_t *settings)
{
	const char *key = edit_key(settings);
	if (strcmp(key, f->edit_owner) == 0)
		return;
	pthread_mutex_lock(&f->settings_lock);
	vf_store_save_edit_if_changed(settings, f->edit_owner);
	vf_store_load_edit(settings);
	pthread_mutex_unlock(&f->settings_lock);
	set_edit_owner(f, key);
}

static bool edit_target_changed(void *priv, obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	struct vf_filter *f = priv;
	f->last_editor_ns = os_gettime_ns();
	bool per = obs_data_get_bool(settings, "per_scene");
	obs_property_set_visible(obs_properties_get(props, "edit_scene"), per);
	obs_property_set_visible(obs_properties_get(props, "anim_group"), per);
	bool scene_edit = edit_key(settings)[0] != '\0';
	obs_property_set_visible(obs_properties_get(props, "reset_default"), scene_edit);
	for (size_t i = 0; i < sizeof(apply_buttons) / sizeof(apply_buttons[0]); i++)
		obs_property_set_visible(obs_properties_get(props, apply_buttons[i]), per);
	switch_edit_profile(f, settings);
	return true;
}

static bool apply_button(struct vf_filter *f, void (*op)(obs_data_t *))
{
	f->last_editor_ns = os_gettime_ns();
	obs_data_t *settings = obs_source_get_settings(f->self);
	pthread_mutex_lock(&f->settings_lock);
	op(settings);
	pthread_mutex_unlock(&f->settings_lock);
	obs_data_release(settings);
	obs_source_update(f->self, NULL);
	return true;
}

/* "Apply to all scenes" under each block: its values from the edited profile go to every profile. */
static bool apply_group(struct vf_filter *f, enum vf_group g)
{
	f->last_editor_ns = os_gettime_ns();
	obs_data_t *settings = obs_source_get_settings(f->self);
	pthread_mutex_lock(&f->settings_lock);
	vf_store_apply_group(settings, g);
	pthread_mutex_unlock(&f->settings_lock);
	obs_data_release(settings);
	obs_source_update(f->self, NULL);
	return true;
}

static bool apply_crop_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_group(data, VF_GROUP_CROP);
}

static bool apply_corners_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_group(data, VF_GROUP_CORNERS);
}

static bool apply_in_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_group(data, VF_GROUP_INNER);
}

static bool apply_out_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_group(data, VF_GROUP_OUTER);
}

static bool apply_bk_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_group(data, VF_GROUP_BACKING);
}

static bool apply_sh_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_group(data, VF_GROUP_SHADOW);
}

static bool apply_content_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_group(data, VF_GROUP_CONTENT);
}

static bool apply_edge_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_group(data, VF_GROUP_EDGE);
}

static void add_apply_button(obs_properties_t *g, const char *name, obs_property_clicked_t cb, struct vf_filter *f)
{
	obs_property_t *b = obs_properties_add_button2(g, name, obs_module_text("Group.ApplyAll"), cb, f);
	obs_property_set_long_description(b, obs_module_text("Group.ApplyAllTip"));
}

static bool reset_default_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(props);
	return apply_button(data, vf_store_reset_edit);
}

static const char *scene_name(const char *uuid, char *buf, size_t n)
{
	obs_source_t *s = obs_get_source_by_uuid(uuid);
	/* a long scene name is cut to the buffer; gcc warns about an unchecked %s truncation */
	if (snprintf(buf, n, "%s", s ? obs_source_get_name(s) : uuid) >= (int)n)
		buf[n - 1] = '\0';
	obs_source_release(s);
	return buf;
}

/* Profiles of scenes that no longer exist (profile-prune.c, at collection load). */
void vf_frame_prune(obs_source_t *filter, const char *const *known, size_t n)
{
	struct vf_filter *f = obs_obj_get_data(filter);
	if (!f)
		return;
	obs_data_t *settings = obs_source_get_settings(f->self);
	pthread_mutex_lock(&f->settings_lock);
	if (vf_store_drop_unknown(settings, known, n)) {
		/* the edited scene is gone: its fields are not saved anywhere, the editor goes back to the default */
		vf_store_load_edit(settings);
		set_edit_owner(f, "");
		pthread_mutex_unlock(&f->settings_lock);
		obs_source_update(f->self, NULL);
	} else {
		pthread_mutex_unlock(&f->settings_lock);
	}
	obs_data_release(settings);
}

struct last_ctx {
	obs_source_t *self;
	bool covered;
};

static void find_filter_after(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct last_ctx *c = param;
	if (obs_filter_get_target(child) == c->self)
		c->covered = true;
}

static void add_warnings(obs_properties_t *props, struct vf_filter *f)
{
	obs_source_t *parent = obs_filter_get_parent(f->self);
	if (!parent)
		return;

	char uuids[16][VF_UUID_LEN];
	size_t n = vf_registry_texture_scenes(parent, uuids, 16);
	if (parent_is_scene(parent)) {
		obs_property_t *w = obs_properties_add_text(props, "warn_scene", obs_module_text("Warning.SceneSource"),
							    OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
	} else if (n) {
		struct dstr names = {0};
		char buf[256];
		for (size_t i = 0; i < n; i++) {
			if (i)
				dstr_cat(&names, ", ");
			dstr_cat(&names, scene_name(uuids[i], buf, sizeof(buf)));
		}
		struct dstr text = {0};
		dstr_printf(&text, obs_module_text("Warning.TexturePath"), names.array);
		obs_property_t *w = obs_properties_add_text(props, "warn_texture", text.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
		dstr_free(&text);
		dstr_free(&names);
	}

	struct last_ctx c = {f->self, false};
	obs_source_enum_filters(parent, find_filter_after, &c);
	if (c.covered) {
		obs_property_t *w = obs_properties_add_text(props, "warn_not_last", obs_module_text("Warning.NotLast"),
							    OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
	}
}

static void add_profile_section(obs_properties_t *props, struct vf_filter *f)
{
	obs_property_t *per = obs_properties_add_bool(props, "per_scene", obs_module_text("Profiles.PerScene"));
	obs_property_set_modified_callback2(per, edit_target_changed, f);

	obs_property_t *list = obs_properties_add_list(props, "edit_scene", obs_module_text("Profiles.Edit"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("Profiles.Default"), "");
	obs_source_t *parent = obs_filter_get_parent(f->self);
	char uuids[64][VF_UUID_LEN];
	size_t n = parent ? vf_registry_scene_uuids(parent, uuids, 64) : 0;
	for (size_t i = 0; i < n; i++) {
		char buf[256];
		obs_property_list_add_string(list, scene_name(uuids[i], buf, sizeof(buf)), uuids[i]);
	}
	obs_property_set_modified_callback2(list, edit_target_changed, f);

	obs_property_t *reset = obs_properties_add_button2(
		props, "reset_default", obs_module_text("Profiles.ResetDefault"), reset_default_clicked, f);
	obs_property_set_long_description(reset, obs_module_text("Profiles.ResetDefaultTip"));

	obs_properties_t *anim = obs_properties_create();
	obs_property_t *on = obs_properties_add_bool(anim, "anim_on", obs_module_text("Anim.On"));
	obs_property_set_long_description(on, obs_module_text("Anim.OnTip"));
	obs_property_set_modified_callback(on, anim_on_changed);
	obs_property_t *ms =
		obs_properties_add_int_slider(anim, "anim_ms", obs_module_text("Anim.CutDuration"), 0, 5000, 10);
	obs_property_int_set_suffix(ms, obs_module_text("Unit.Ms"));
	obs_property_set_long_description(ms, obs_module_text("Anim.CutDurationTip"));
	obs_properties_add_group(props, "anim_group", obs_module_text("Anim"), OBS_GROUP_NORMAL, anim);
}

/* A newly opened editor starts on the scene being worked on: the preview scene in Studio Mode, else program. */
/* UI thread. Puts the editor on the scene being worked on (preview in Studio Mode, else program) if the source is in
 * it; the fields of the profile left are saved first if they were edited. reload: update the filter too (not needed
 * for drawing: the store gives the same values either way, so a scene switch skips it). Returns true when the edited
 * profile changed. */
static bool select_current_scene(struct vf_filter *f, bool reload)
{
	obs_source_t *parent = obs_filter_get_parent(f->self);
	obs_data_t *settings = obs_source_get_settings(f->self);
	bool changed = false;
	if (parent && obs_data_get_bool(settings, "per_scene")) {
		struct vf_anim_state as;
		vf_anim_get(&as);
		const char *want = as.preview_uuid[0] ? as.preview_uuid : as.program_uuid;
		char uuids[64][VF_UUID_LEN];
		size_t n = vf_registry_scene_uuids(parent, uuids, 64);
		for (size_t i = 0; i < n && !changed; i++) {
			if (strcmp(uuids[i], want) != 0)
				continue;
			if (strcmp(obs_data_get_string(settings, "edit_scene"), want) == 0)
				break;
			pthread_mutex_lock(&f->settings_lock);
			obs_data_set_string(settings, "edit_scene", want);
			pthread_mutex_unlock(&f->settings_lock);
			switch_edit_profile(f, settings);
			if (reload)
				obs_source_update(f->self, NULL);
			changed = true;
		}
	}
	obs_data_release(settings);
	return changed;
}

/* A scene switch: an open properties dialog follows it (frame-main.c). */
void vf_frame_follow_scene(obs_source_t *filter)
{
	struct vf_filter *f = obs_obj_get_data(filter);
	if (f && select_current_scene(f, false))
		obs_source_update_properties(filter);
}

/* Counts the live properties of a filter. The dialog may outlive the filter, so it holds a weak reference. */
struct editor_token {
	obs_weak_source_t *filter;
};

static void editor_closed(void *param)
{
	struct editor_token *t = param;
	obs_source_t *source = obs_weak_source_get_source(t->filter);
	struct vf_filter *f = source ? obs_obj_get_data(source) : NULL;
	if (f)
		os_atomic_dec_long(&f->editors);
	obs_source_release(source);
	obs_weak_source_release(t->filter);
	bfree(t);
}

static obs_properties_t *vf_get_properties(void *data)
{
	struct vf_filter *f = data;
	obs_properties_t *props = obs_properties_create();
	struct editor_token *token = bzalloc(sizeof(*token));
	token->filter = obs_source_get_weak_source(f->self);
	os_atomic_inc_long(&f->editors);
	obs_properties_set_param(props, token, editor_closed);

	uint64_t now = os_gettime_ns();
	obs_data_t *settings = obs_source_get_settings(f->self);
	pthread_mutex_lock(&f->settings_lock);
	vf_store_migrate(settings);
	if (parent_is_scene(obs_filter_get_parent(f->self)))
		vf_store_drop_outer(settings); /* the boxes show unchecked what cannot be drawn there */
	pthread_mutex_unlock(&f->settings_lock);
	obs_data_release(settings);

	if (now - f->last_editor_ns > 1500000000ULL) /* only a newly opened editor jumps to the current scene */
		select_current_scene(f, true);
	f->last_editor_ns = now;

	add_warnings(props, f);
	add_profile_section(props, f);

	obs_properties_t *corners = obs_properties_create();
	add_block_switch(corners, "co_on", "Corners.On");
	obs_property_t *per = obs_properties_add_bool(corners, "per_corner", obs_module_text("Corners.PerCorner"));
	obs_property_set_modified_callback(per, blocks_changed);
	add_px(corners, "radius", "Corners.Radius", 0, 1000);
	add_px(corners, "r_tl", "Corners.TL", 0, 1000);
	add_px(corners, "r_tr", "Corners.TR", 0, 1000);
	add_px(corners, "r_br", "Corners.BR", 0, 1000);
	add_px(corners, "r_bl", "Corners.BL", 0, 1000);
	add_apply_button(corners, "corners_apply", apply_corners_clicked, f);
	obs_properties_add_group(props, "corners_group", obs_module_text("Corners"), OBS_GROUP_NORMAL, corners);

	add_stroke_group(props, "in", "Inner", true, apply_in_clicked, f);
	add_stroke_group(props, "out", "Outer", true, apply_out_clicked, f);
	const bool on_scene = parent_is_scene(obs_filter_get_parent(f->self));
	if (on_scene)
		obs_property_set_enabled(obs_properties_get(props, "out_group"), false);
	add_stroke_group(props, "bk", "Backing", false, apply_bk_clicked, f);

	obs_properties_t *sh = obs_properties_create();
	obs_property_t *on = obs_properties_add_bool(sh, "sh_on", obs_module_text("Shadow.On"));
	obs_property_set_modified_callback(on, shadow_on_changed);
	obs_property_t *type = obs_properties_add_list(sh, "sh_type", obs_module_text("Shadow.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, obs_module_text("Shadow.Type.Outer"), VF_SHADOW_OUTER);
	obs_property_list_add_int(type, obs_module_text("Shadow.Type.Inner"), VF_SHADOW_INNER);
	obs_property_list_item_disable(type, 0, on_scene);
	obs_properties_add_color(sh, "sh_color", obs_module_text("Shadow.Color"));
	add_percent(sh, "sh_opacity", "Opacity", 0, 100);
	add_px(sh, "sh_blur", "Shadow.Blur", 0, 300);
	add_px(sh, "sh_x", "Shadow.OffsetX", -500, 500);
	add_px(sh, "sh_y", "Shadow.OffsetY", -500, 500);
	add_apply_button(sh, "sh_apply", apply_sh_clicked, f);
	obs_properties_add_group(props, "sh_group", obs_module_text("Shadow"), OBS_GROUP_NORMAL, sh);

	obs_properties_t *edge = obs_properties_create();
	add_block_switch(edge, "ed_on", "Edge.On");
	obs_property_t *per_side = obs_properties_add_bool(edge, "ed_per_side", obs_module_text("Edge.PerSide"));
	obs_property_set_modified_callback(per_side, blocks_changed);
	static const char *edge_keys[5][2] = {{"ed_w", "Edge.Width"},
					      {"ed_l", "Crop.Left"},
					      {"ed_t", "Crop.Top"},
					      {"ed_r", "Crop.Right"},
					      {"ed_b", "Crop.Bottom"}};
	for (int i = 0; i < 5; i++)
		add_unit(edge, edge_keys[i][0], edge_keys[i][1], 0, 4000, " px", "Edge.Tip");
	add_apply_button(edge, "edge_apply", apply_edge_clicked, f);
	obs_properties_add_group(props, "edge_group", obs_module_text("Edge"), OBS_GROUP_NORMAL, edge);

	obs_properties_t *crop = obs_properties_create();
	add_block_switch(crop, "crop_on", "Crop.On");
	static const char *crop_keys[4][2] = {{"crop_l", "Crop.Left"},
					      {"crop_t", "Crop.Top"},
					      {"crop_r", "Crop.Right"},
					      {"crop_b", "Crop.Bottom"}};
	for (int i = 0; i < 4; i++)
		add_unit(crop, crop_keys[i][0], crop_keys[i][1], 0, 16384, " px", "Crop.Tip");
	add_apply_button(crop, "crop_apply", apply_crop_clicked, f);
	obs_properties_add_group(props, "crop_group", obs_module_text("Crop"), OBS_GROUP_NORMAL, crop);

	obs_properties_t *content = obs_properties_create();
	add_block_switch(content, "ct_on", "Content.On");
	add_percent(content, "zoom", "Content.Zoom", 10, 500);
	add_px(content, "cx", "Content.OffsetX", -2000, 2000);
	add_px(content, "cy", "Content.OffsetY", -2000, 2000);
	add_apply_button(content, "content_apply", apply_content_clicked, f);
	obs_properties_add_group(props, "content_group", obs_module_text("Content"), OBS_GROUP_NORMAL, content);

	vf_add_about(props, "UltimateFrame");
	return props;
}

struct obs_source_info vf_frame_filter_info = {
	.id = "vakovsky_ultimate_frame",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = vf_get_name,
	.create = vf_create,
	.destroy = vf_destroy,
	.update = vf_update,
	.get_defaults = vf_defaults,
	.get_properties = vf_get_properties,
	.video_render = vf_video_render,
	.video_tick = vf_video_tick,
	.get_width = vf_get_width,
	.get_height = vf_get_height,
	.video_get_color_space = vf_get_color_space,
};
