#include "vf-store.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <util/bmem.h>
#include <util/darray.h>

/* ---------- colors: OBS stores 0xAABBGGRR sRGB, params hold linear ---------- */

static float srgb_to_linear(float c)
{
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c)
{
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static struct vf_color color_from_int(long long v)
{
	uint32_t u = (uint32_t)v;
	struct vf_color c;
	c.r = srgb_to_linear((float)(u & 0xFF) / 255.0f);
	c.g = srgb_to_linear((float)((u >> 8) & 0xFF) / 255.0f);
	c.b = srgb_to_linear((float)((u >> 16) & 0xFF) / 255.0f);
	c.a = (float)((u >> 24) & 0xFF) / 255.0f;
	return c;
}

static uint32_t to_byte(float v)
{
	v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	return (uint32_t)lroundf(v * 255.0f);
}

static long long color_to_int(struct vf_color c)
{
	return (long long)(to_byte(linear_to_srgb(c.r)) | (to_byte(linear_to_srgb(c.g)) << 8) |
			   (to_byte(linear_to_srgb(c.b)) << 16) | (to_byte(c.a) << 24));
}

/* ---------- flat keys ---------- */

typedef void (*set_num_fn)(obs_data_t *, const char *, double);
typedef void (*set_int_fn)(obs_data_t *, const char *, long long);
typedef void (*set_bool_fn)(obs_data_t *, const char *, bool);

struct writer {
	set_num_fn num;
	set_int_fn integer;
	set_bool_fn boolean;
};

/* Values come from floats (1.9f * 100 is 189.99999): drop that noise, the editor shows whole units. */
static double clean(double v)
{
	return round(v * 10000.0) / 10000.0;
}

static void set_double_clean(obs_data_t *d, const char *k, double v)
{
	obs_data_set_double(d, k, clean(v));
}

static void set_default_double_clean(obs_data_t *d, const char *k, double v)
{
	obs_data_set_default_double(d, k, clean(v));
}

static const struct writer W_VALUE = {set_double_clean, obs_data_set_int, obs_data_set_bool};
static const struct writer W_DEFAULT = {set_default_double_clean, obs_data_set_default_int, obs_data_set_default_bool};

static void key(char *buf, size_t n, const char *prefix, const char *name)
{
	snprintf(buf, n, "%s_%s", prefix, name);
}

/* The fill of a stroke or of the backing: opacity, solid or gradient, colors, angle. */
static void fill_read(obs_data_t *d, const char *pre, struct vf_stroke *s)
{
	char k[32];
	key(k, sizeof(k), pre, "opacity");
	s->opacity = (float)obs_data_get_double(d, k) / 100.0f;
	key(k, sizeof(k), pre, "fill");
	s->fill = obs_data_get_int(d, k) == VF_FILL_LINEAR ? VF_FILL_LINEAR : VF_FILL_SOLID;
	key(k, sizeof(k), pre, "c0");
	s->c0 = color_from_int(obs_data_get_int(d, k));
	key(k, sizeof(k), pre, "c1");
	s->c1 = color_from_int(obs_data_get_int(d, k));
	key(k, sizeof(k), pre, "angle");
	s->angle_deg = (float)obs_data_get_double(d, k);
}

static void fill_write(const struct writer *w, obs_data_t *d, const char *pre, const struct vf_stroke *s)
{
	char k[32];
	key(k, sizeof(k), pre, "opacity");
	w->num(d, k, s->opacity * 100.0);
	key(k, sizeof(k), pre, "fill");
	w->integer(d, k, s->fill);
	key(k, sizeof(k), pre, "c0");
	w->integer(d, k, color_to_int(s->c0));
	key(k, sizeof(k), pre, "c1");
	w->integer(d, k, color_to_int(s->c1));
	key(k, sizeof(k), pre, "angle");
	w->num(d, k, s->angle_deg);
}

static void stroke_read(obs_data_t *d, const char *pre, struct vf_stroke *s)
{
	char k[32];
	key(k, sizeof(k), pre, "w");
	s->width = (float)obs_data_get_double(d, k);
	key(k, sizeof(k), pre, "soft");
	s->softness = (float)obs_data_get_double(d, k);
	fill_read(d, pre, s);
}

static void stroke_write(const struct writer *w, obs_data_t *d, const char *pre, const struct vf_stroke *s)
{
	char k[32];
	key(k, sizeof(k), pre, "w");
	w->num(d, k, s->width);
	key(k, sizeof(k), pre, "soft");
	w->num(d, k, s->softness);
	fill_write(w, d, pre, s);
}

static void backing_write(const struct writer *w, obs_data_t *d, const struct vf_params *p)
{
	w->boolean(d, "bk_on", p->backing_on);
	fill_write(w, d, "bk", &p->backing);
}

/* Before the backing had a block of its own it was the outer border's checkbox "out_back", drawn in the border's
 * fill and hidden with the border. A layer with that key and without "bk_on" is from then. */
static bool old_backing(obs_data_t *layer)
{
	return obs_data_has_user_value(layer, "out_back") && !obs_data_has_user_value(layer, "bk_on");
}

static void backing_from_old(bool out_back, struct vf_params *p)
{
	p->backing_on = out_back && p->outer_on;
	p->backing = p->outer;
	p->backing.width = 0.0f;
	p->backing.softness = 0.0f;
}

static const char *corner_keys[4] = {"r_tl", "r_tr", "r_br", "r_bl"};

static const char *const crop_keys[4] = {"crop_l", "crop_t", "crop_r", "crop_b"};

static const char *const edge_keys[4] = {"ed_l", "ed_t", "ed_r", "ed_b"};

void vf_params_from_data(obs_data_t *d, struct vf_params *p)
{
	p->corners_on = obs_data_get_bool(d, "co_on");
	p->inner_on = obs_data_get_bool(d, "in_on");
	p->outer_on = obs_data_get_bool(d, "out_on");
	p->crop_on = obs_data_get_bool(d, "crop_on");
	p->content_on = obs_data_get_bool(d, "ct_on");
	p->per_corner = obs_data_get_bool(d, "per_corner");
	p->radius = (float)obs_data_get_double(d, "radius");
	for (int i = 0; i < 4; i++)
		p->corner[i] = (float)obs_data_get_double(d, corner_keys[i]);
	stroke_read(d, "in", &p->inner);
	stroke_read(d, "out", &p->outer);
	p->shadow_on = obs_data_get_bool(d, "sh_on");
	p->shadow_type = obs_data_get_int(d, "sh_type") == VF_SHADOW_INNER ? VF_SHADOW_INNER : VF_SHADOW_OUTER;
	p->shadow_color = color_from_int(obs_data_get_int(d, "sh_color"));
	p->shadow_opacity = (float)obs_data_get_double(d, "sh_opacity") / 100.0f;
	p->shadow_blur = (float)obs_data_get_double(d, "sh_blur");
	p->shadow_x = (float)obs_data_get_double(d, "sh_x");
	p->shadow_y = (float)obs_data_get_double(d, "sh_y");
	p->zoom = (float)obs_data_get_double(d, "zoom") / 100.0f;
	p->content_x = (float)obs_data_get_double(d, "cx");
	p->content_y = (float)obs_data_get_double(d, "cy");
	for (int i = 0; i < 4; i++)
		p->crop[i] = (float)obs_data_get_int(d, crop_keys[i]);
	p->backing_on = obs_data_get_bool(d, "bk_on");
	fill_read(d, "bk", &p->backing);
	p->backing.width = 0.0f;
	p->backing.softness = 0.0f;
	if (old_backing(d))
		backing_from_old(obs_data_get_bool(d, "out_back"), p);
	p->edge_on = obs_data_get_bool(d, "ed_on");
	p->per_side = obs_data_get_bool(d, "ed_per_side");
	p->edge = (float)obs_data_get_double(d, "ed_w");
	for (int i = 0; i < 4; i++)
		p->edge_side[i] = (float)obs_data_get_double(d, edge_keys[i]);
}

static void params_write(const struct writer *w, const struct vf_params *p, obs_data_t *d)
{
	w->boolean(d, "co_on", p->corners_on);
	w->boolean(d, "in_on", p->inner_on);
	w->boolean(d, "out_on", p->outer_on);
	w->boolean(d, "crop_on", p->crop_on);
	w->boolean(d, "ct_on", p->content_on);
	w->boolean(d, "per_corner", p->per_corner);
	w->num(d, "radius", p->radius);
	for (int i = 0; i < 4; i++)
		w->num(d, corner_keys[i], p->corner[i]);
	stroke_write(w, d, "in", &p->inner);
	stroke_write(w, d, "out", &p->outer);
	w->boolean(d, "sh_on", p->shadow_on);
	w->integer(d, "sh_type", p->shadow_type);
	w->integer(d, "sh_color", color_to_int(p->shadow_color));
	w->num(d, "sh_opacity", p->shadow_opacity * 100.0);
	w->num(d, "sh_blur", p->shadow_blur);
	w->num(d, "sh_x", p->shadow_x);
	w->num(d, "sh_y", p->shadow_y);
	w->num(d, "zoom", p->zoom * 100.0);
	w->num(d, "cx", p->content_x);
	w->num(d, "cy", p->content_y);
	for (int i = 0; i < 4; i++)
		w->integer(d, crop_keys[i], lroundf(p->crop[i]));
	backing_write(w, d, p);
	w->boolean(d, "ed_on", p->edge_on);
	w->boolean(d, "ed_per_side", p->per_side);
	w->num(d, "ed_w", p->edge);
	for (int i = 0; i < 4; i++)
		w->num(d, edge_keys[i], p->edge_side[i]);
}

void vf_params_to_data(const struct vf_params *p, obs_data_t *d)
{
	params_write(&W_VALUE, p, d);
}

void vf_params_set_defaults(obs_data_t *d)
{
	struct vf_params def;
	vf_params_default(&def);
	params_write(&W_DEFAULT, &def, d);
}

/* ---------- per-scene profiles ---------- */

#include <string.h>

static long long get_int_or(obs_data_t *d, const char *key, long long fallback)
{
	return obs_data_has_user_value(d, key) || obs_data_has_default_value(d, key) ? obs_data_get_int(d, key)
										     : fallback;
}

void vf_store_set_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "per_scene", false);
	obs_data_set_default_string(settings, "edit_scene", "");
	obs_data_set_default_bool(settings, "anim_on", true);
	obs_data_set_default_int(settings, "anim_ms", 400);
}

/* An old layer's backing, from the outer border as it is after that layer: an old scene profile over a new default
 * profile keeps its own backing instead of inheriting the default's "bk_on". */
static void apply_layer(obs_data_t *tmp, obs_data_t *layer)
{
	obs_data_apply(tmp, layer);
	if (!old_backing(layer))
		return;
	struct vf_params p;
	vf_params_from_data(tmp, &p);
	backing_from_old(obs_data_get_bool(layer, "out_back"), &p);
	backing_write(&W_VALUE, tmp, &p);
	obs_data_erase(tmp, "out_back");
}

/* Profile values layered over the hard defaults and then over the default profile (scene profiles inherit). */
static void parse_profile(obs_data_t *def_obj, obs_data_t *scene_obj, struct vf_params *out)
{
	obs_data_t *tmp = obs_data_create();
	vf_params_set_defaults(tmp);
	if (def_obj)
		apply_layer(tmp, def_obj);
	if (scene_obj)
		apply_layer(tmp, scene_obj);
	vf_params_from_data(tmp, out);
	obs_data_release(tmp);
}

static const char *edited_scene(obs_data_t *settings)
{
	const char *u = obs_data_get_string(settings, "edit_scene");
	return obs_data_get_bool(settings, "per_scene") && u && *u ? u : NULL;
}

/* before the checkbox there was a mode list: "as the OBS transition" and "own duration" both animate */
static bool anim_on_of(obs_data_t *settings)
{
	if (obs_data_has_user_value(settings, "anim_on"))
		return obs_data_get_bool(settings, "anim_on");
	return !obs_data_has_user_value(settings, "anim_mode") ||
	       obs_data_get_int(settings, "anim_mode") != VF_ANIM_OFF;
}

/* Rewrites an old profile's backing as the backing block's own keys, with the values it is drawn with now. */
static void migrate_backing(obs_data_t *prof, obs_data_t *def_obj, obs_data_t *scene_obj)
{
	if (!old_backing(prof))
		return;
	struct vf_params p;
	parse_profile(def_obj, scene_obj, &p);
	backing_write(&W_VALUE, prof, &p);
	obs_data_erase(prof, "out_back");
}

void vf_store_migrate(obs_data_t *settings)
{
	if (!obs_data_has_user_value(settings, "anim_on") && obs_data_has_user_value(settings, "anim_mode"))
		obs_data_set_bool(settings, "anim_on", anim_on_of(settings));

	obs_data_t *def_obj = obs_data_get_obj(settings, "default");
	obs_data_t *scenes = obs_data_get_obj(settings, "scenes");
	/* scene profiles first: they are parsed over the default profile as it was */
	for (obs_data_item_t *it = scenes ? obs_data_first(scenes) : NULL; it; obs_data_item_next(&it)) {
		obs_data_t *prof = obs_data_item_get_obj(it);
		if (prof)
			migrate_backing(prof, def_obj, prof);
		obs_data_release(prof);
	}
	if (def_obj)
		migrate_backing(def_obj, def_obj, NULL);
	/* the flat fields: the edited scene profile over the default one, or the default profile itself */
	migrate_backing(settings, edited_scene(settings) ? def_obj : settings,
			edited_scene(settings) ? settings : NULL);
	obs_data_release(scenes);
	obs_data_release(def_obj);
}

/* Writes the layer's outer border and shadow switches off, keeping an inner shadow as it is drawn (p, parsed before). */
static bool drop_outer_layer(obs_data_t *layer, const struct vf_params *p)
{
	const bool outer_shadow = p->shadow_on && p->shadow_type == VF_SHADOW_OUTER;
	obs_data_set_bool(layer, "out_on", false);
	obs_data_set_bool(layer, "sh_on", p->shadow_on && !outer_shadow);
	return p->outer_on || outer_shadow;
}

bool vf_store_drop_outer(obs_data_t *settings)
{
	obs_data_t *def_obj = obs_data_get_obj(settings, "default");
	obs_data_t *scenes = obs_data_get_obj(settings, "scenes");
	const bool edited = edited_scene(settings) != NULL;
	struct vf_params flat;
	parse_profile(edited ? def_obj : settings, edited ? settings : NULL, &flat);
	bool changed = false;
	/* scene profiles first: they are parsed over the default profile as it was */
	for (obs_data_item_t *it = scenes ? obs_data_first(scenes) : NULL; it; obs_data_item_next(&it)) {
		obs_data_t *prof = obs_data_item_get_obj(it);
		if (prof) {
			struct vf_params p;
			parse_profile(def_obj, prof, &p);
			changed = drop_outer_layer(prof, &p) || changed;
		}
		obs_data_release(prof);
	}
	if (def_obj) {
		struct vf_params p;
		parse_profile(def_obj, NULL, &p);
		changed = drop_outer_layer(def_obj, &p) || changed;
	}
	changed = drop_outer_layer(settings, &flat) || changed;
	obs_data_release(scenes);
	obs_data_release(def_obj);
	return changed;
}

void vf_store_load(obs_data_t *settings, struct vf_store *out)
{
	memset(out, 0, sizeof(*out));
	out->per_scene = obs_data_get_bool(settings, "per_scene");
	out->anim_on = anim_on_of(settings);
	out->anim_ms = (int)get_int_or(settings, "anim_ms", 400);

	/* The flat fields are the live value of the edited profile (saved into it only when the editor switches
	 * profiles, on the UI thread): update runs on the graphics thread and must not write settings. */
	const char *edit = edited_scene(settings);
	obs_data_t *def_obj = obs_data_get_obj(settings, "default");
	parse_profile(edit ? def_obj : settings, NULL, &out->def);

	obs_data_t *scenes = obs_data_get_obj(settings, "scenes");
	size_t n = 0;
	for (obs_data_item_t *it = scenes ? obs_data_first(scenes) : NULL; it; obs_data_item_next(&it))
		n++;
	if (n || edit) {
		out->scenes = bzalloc(sizeof(*out->scenes) * (n + 1));
		for (obs_data_item_t *it = n ? obs_data_first(scenes) : NULL; it; obs_data_item_next(&it)) {
			obs_data_t *prof = obs_data_item_get_obj(it);
			if (!prof)
				continue;
			const char *uuid = obs_data_item_get_name(it);
			if (!edit || strcmp(uuid, edit) != 0) {
				struct vf_profile *p = &out->scenes[out->count++];
				snprintf(p->uuid, sizeof(p->uuid), "%s", uuid);
				parse_profile(def_obj, prof, &p->params);
			}
			obs_data_release(prof);
		}
		if (edit) {
			struct vf_profile *p = &out->scenes[out->count++];
			snprintf(p->uuid, sizeof(p->uuid), "%s", edit);
			parse_profile(def_obj, settings, &p->params);
		}
	}
	obs_data_release(scenes);
	obs_data_release(def_obj);

	/* the store is only drawn: a switched-off block is its zero state, so a switch animates through it */
	vf_params_effective(&out->def);
	for (size_t i = 0; i < out->count; i++)
		vf_params_effective(&out->scenes[i].params);
}

void vf_store_free(struct vf_store *s)
{
	bfree(s->scenes);
	s->scenes = NULL;
	s->count = 0;
}

const struct vf_params *vf_store_find(const struct vf_store *s, const char *uuid)
{
	for (size_t i = 0; uuid && i < s->count; i++)
		if (strcmp(s->scenes[i].uuid, uuid) == 0)
			return &s->scenes[i].params;
	return &s->def;
}

void vf_store_save_edit(obs_data_t *settings)
{
	const char *uuid = edited_scene(settings);
	vf_store_save_edit_to(settings, uuid ? uuid : "");
}

void vf_store_save_edit_to(obs_data_t *settings, const char *key)
{
	struct vf_params p;
	vf_params_from_data(settings, &p);
	obs_data_t *prof = obs_data_create();
	vf_params_to_data(&p, prof);

	if (key && *key) {
		obs_data_t *scenes = obs_data_get_obj(settings, "scenes");
		if (!scenes)
			scenes = obs_data_create();
		obs_data_set_obj(scenes, key, prof);
		obs_data_set_obj(settings, "scenes", scenes);
		obs_data_release(scenes);
	} else {
		obs_data_set_obj(settings, "default", prof);
	}
	obs_data_release(prof);
}

static char *params_json(const struct vf_params *p)
{
	obs_data_t *d = obs_data_create();
	vf_params_to_data(p, d);
	char *json = bstrdup(obs_data_get_json(d));
	obs_data_release(d);
	return json;
}

bool vf_store_save_edit_if_changed(obs_data_t *settings, const char *key)
{
	obs_data_t *def_obj = obs_data_get_obj(settings, "default");
	obs_data_t *scenes = key && *key ? obs_data_get_obj(settings, "scenes") : NULL;
	obs_data_t *scene_obj = scenes ? obs_data_get_obj(scenes, key) : NULL;
	struct vf_params stored, edited;
	parse_profile(key && *key ? def_obj : NULL, key && *key ? scene_obj : def_obj, &stored);
	vf_params_from_data(settings, &edited);
	obs_data_release(scene_obj);
	obs_data_release(scenes);
	obs_data_release(def_obj);

	char *a = params_json(&stored), *b = params_json(&edited);
	bool changed = strcmp(a, b) != 0;
	bfree(a);
	bfree(b);
	if (changed)
		vf_store_save_edit_to(settings, key);
	return changed;
}

void vf_store_load_edit(obs_data_t *settings)
{
	const char *uuid = edited_scene(settings);
	obs_data_t *def_obj = obs_data_get_obj(settings, "default");
	obs_data_t *scenes = uuid ? obs_data_get_obj(settings, "scenes") : NULL;
	obs_data_t *scene_obj = scenes ? obs_data_get_obj(scenes, uuid) : NULL;

	struct vf_params p;
	parse_profile(def_obj, scene_obj, &p);
	vf_params_to_data(&p, settings);

	obs_data_release(scene_obj);
	obs_data_release(scenes);
	obs_data_release(def_obj);
}

bool vf_store_drop_unknown(obs_data_t *settings, const char *const *known_uuids, size_t n)
{
	obs_data_t *scenes = obs_data_get_obj(settings, "scenes");
	if (scenes) {
		/* collect first: erasing while iterating invalidates the iterator */
		DARRAY(char *) drop;
		da_init(drop);
		for (obs_data_item_t *it = obs_data_first(scenes); it; obs_data_item_next(&it)) {
			const char *name = obs_data_item_get_name(it);
			bool known = false;
			for (size_t i = 0; i < n && !known; i++)
				known = strcmp(known_uuids[i], name) == 0;
			if (!known) {
				char *copy = bstrdup(name);
				da_push_back(drop, &copy);
			}
		}
		for (size_t i = 0; i < drop.num; i++) {
			obs_data_erase(scenes, drop.array[i]);
			bfree(drop.array[i]);
		}
		da_free(drop);
		obs_data_release(scenes);
	}

	const char *edit = obs_data_get_string(settings, "edit_scene");
	bool edit_known = !edit || !*edit;
	for (size_t i = 0; i < n && !edit_known; i++)
		edit_known = strcmp(known_uuids[i], edit) == 0;
	if (!edit_known)
		obs_data_set_string(settings, "edit_scene", "");
	return !edit_known;
}

/* Rewrites a stored profile with one block taken from src. */
static void apply_group_to(obs_data_t *def_obj, obs_data_t *prof, const struct vf_params *src, enum vf_group g)
{
	struct vf_params p;
	parse_profile(prof == def_obj ? NULL : def_obj, prof, &p);
	vf_params_copy_group(&p, src, g);
	vf_params_to_data(&p, prof);
}

void vf_store_apply_group(obs_data_t *settings, enum vf_group g)
{
	struct vf_params src;
	vf_params_from_data(settings, &src);
	vf_store_save_edit(settings); /* the edited profile may exist only in the flat fields so far */

	obs_data_t *def_obj = obs_data_get_obj(settings, "default");
	if (!def_obj) {
		def_obj = obs_data_create();
		obs_data_set_obj(settings, "default", def_obj);
	}
	obs_data_t *scenes = obs_data_get_obj(settings, "scenes");
	/* scene profiles first: they are parsed over the default profile as it was */
	for (obs_data_item_t *it = scenes ? obs_data_first(scenes) : NULL; it; obs_data_item_next(&it)) {
		obs_data_t *prof = obs_data_item_get_obj(it);
		if (prof)
			apply_group_to(def_obj, prof, &src, g);
		obs_data_release(prof);
	}
	apply_group_to(def_obj, def_obj, &src, g);
	obs_data_release(scenes);
	obs_data_release(def_obj);
}

void vf_store_reset_edit(obs_data_t *settings)
{
	const char *uuid = edited_scene(settings);
	obs_data_t *scenes = uuid ? obs_data_get_obj(settings, "scenes") : NULL;
	if (scenes) {
		obs_data_erase(scenes, uuid);
		obs_data_release(scenes);
	}
	vf_store_load_edit(settings);
}
