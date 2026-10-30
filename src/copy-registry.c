#include "copy-registry.h"
#include "vf-select.h"

#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdlib.h>
#include <string.h>

#include <plugin-support.h>

struct pair {
	obs_source_t *source;
	struct vf_reg_copy copy;
};

struct reg_entry {
	obs_source_t *source;
	size_t first, count;
};

static struct {
	pthread_mutex_t mutex; /* guards the arrays for readers outside the graphics thread */
	struct vf_reg_copy *copies;
	size_t ncopies;
	struct reg_entry *entries;
	size_t nentries;
	volatile bool dirty;
	uint64_t generation;
	bool initialized;
} reg;

/* ---------- signals: they only mark the table dirty ---------- */

static void on_scene_changed(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	vf_registry_mark_dirty();
}

static bool is_scene_like(obs_source_t *s)
{
	return s && (obs_source_is_scene(s) || obs_source_is_group(s));
}

static const char *scene_signals[] = {"item_add", "item_remove", "reorder", "refresh"};

static void connect_scene(obs_source_t *s)
{
	signal_handler_t *sh = obs_source_get_signal_handler(s);
	for (size_t i = 0; i < sizeof(scene_signals) / sizeof(*scene_signals); i++)
		signal_handler_connect(sh, scene_signals[i], on_scene_changed, NULL);
}

static void disconnect_scene(obs_source_t *s)
{
	signal_handler_t *sh = obs_source_get_signal_handler(s);
	for (size_t i = 0; i < sizeof(scene_signals) / sizeof(*scene_signals); i++)
		signal_handler_disconnect(sh, scene_signals[i], on_scene_changed, NULL);
}

static void on_source_create(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	obs_source_t *s = calldata_ptr(cd, "source");
	if (is_scene_like(s)) {
		connect_scene(s);
		vf_registry_mark_dirty();
	}
}

static void on_source_gone(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	if (is_scene_like(calldata_ptr(cd, "source")))
		vf_registry_mark_dirty();
}

static bool connect_existing(void *param, obs_source_t *s)
{
	if (is_scene_like(s)) {
		if (param)
			connect_scene(s);
		else
			disconnect_scene(s);
	}
	return true;
}

/* ---------- table ---------- */

static void swap_out_and_release(struct vf_reg_copy *new_copies, size_t ncopies, struct reg_entry *new_entries,
				 size_t nentries)
{
	pthread_mutex_lock(&reg.mutex);
	struct vf_reg_copy *old = reg.copies;
	size_t nold = reg.ncopies;
	bfree(reg.entries);
	reg.copies = new_copies;
	reg.ncopies = ncopies;
	reg.entries = new_entries;
	reg.nentries = nentries;
	reg.generation++;
	pthread_mutex_unlock(&reg.mutex);

	for (size_t i = 0; i < nold; i++)
		obs_sceneitem_release(old[i].item);
	bfree(old);
}

struct build_ctx {
	DARRAY(struct pair) pairs;
	const char *scene_uuid;
	obs_sceneitem_t *group;
};

static bool collect_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct build_ctx *ctx = param;

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_t *prev = ctx->group;
		ctx->group = item;
		obs_sceneitem_group_enum_items(item, collect_item, ctx);
		ctx->group = prev;
	}

	struct pair *p = da_push_back_new(ctx->pairs);
	p->source = obs_sceneitem_get_source(item);
	obs_sceneitem_addref(item);
	p->copy.item = item;
	p->copy.group = ctx->group;
	snprintf(p->copy.scene_uuid, VF_UUID_LEN, "%s", ctx->scene_uuid);
	return true;
}

static bool collect_scene(void *param, obs_source_t *scene_src)
{
	struct build_ctx *ctx = param;
	ctx->scene_uuid = obs_source_get_uuid(scene_src);
	ctx->group = NULL;
	obs_scene_enum_items(obs_scene_from_source(scene_src), collect_item, ctx);
	return true;
}

static int cmp_pair(const void *a, const void *b)
{
	uintptr_t x = (uintptr_t)((const struct pair *)a)->source;
	uintptr_t y = (uintptr_t)((const struct pair *)b)->source;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static void rebuild(void)
{
	uint64_t t0 = os_gettime_ns();
	struct build_ctx ctx = {0};
	da_init(ctx.pairs);
	obs_enum_scenes(collect_scene, &ctx);

	size_t n = ctx.pairs.num;
	if (n)
		qsort(ctx.pairs.array, n, sizeof(struct pair), cmp_pair);

	struct vf_reg_copy *copies = n ? bmalloc(sizeof(*copies) * n) : NULL;
	struct reg_entry *entries = n ? bmalloc(sizeof(*entries) * n) : NULL;
	size_t nentries = 0;
	for (size_t i = 0; i < n; i++) {
		copies[i] = ctx.pairs.array[i].copy;
		if (!nentries || entries[nentries - 1].source != ctx.pairs.array[i].source)
			entries[nentries++] = (struct reg_entry){ctx.pairs.array[i].source, i, 0};
		entries[nentries - 1].count++;
	}
	da_free(ctx.pairs);

	/* tick callbacks run outside the graphics context; any render (also from other threads, e.g. screenshots)
	 * holds it, so the swap under the graphics lock never frees arrays a render is reading */
	obs_enter_graphics();
	swap_out_and_release(copies, n, entries, nentries);
	obs_leave_graphics();

	(void)t0;
}

/* Called on the UI thread: the graphics lock keeps the render thread off the arrays while they go away. */
static void release_now(void)
{
	obs_enter_graphics();
	swap_out_and_release(NULL, 0, NULL, 0);
	obs_leave_graphics();
}

/* OBS drops every frontend callback before modules unload; removing ours later only logs a warning. */
static bool frontend_hooked;

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		release_now();
		vf_registry_mark_dirty();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		release_now();
		vf_registry_mark_dirty();
		/* safe here: the frontend walks its callbacks from the end */
		obs_frontend_remove_event_callback(on_frontend_event, NULL);
		frontend_hooked = false;
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		vf_registry_mark_dirty();
		break;
	default:
		break;
	}
}

/* ---------- public ---------- */

void vf_registry_init(void)
{
	pthread_mutex_init(&reg.mutex, NULL);
	reg.initialized = true;
	signal_handler_t *core = obs_get_signal_handler();
	signal_handler_connect(core, "source_create", on_source_create, NULL);
	signal_handler_connect(core, "source_destroy", on_source_gone, NULL);
	signal_handler_connect(core, "source_remove", on_source_gone, NULL);
	obs_enum_all_sources(connect_existing, (void *)1);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	frontend_hooked = true;
	vf_registry_mark_dirty();
}

void vf_registry_free(void)
{
	if (!reg.initialized)
		return;
	if (frontend_hooked)
		obs_frontend_remove_event_callback(on_frontend_event, NULL);
	frontend_hooked = false;
	signal_handler_t *core = obs_get_signal_handler();
	signal_handler_disconnect(core, "source_create", on_source_create, NULL);
	signal_handler_disconnect(core, "source_destroy", on_source_gone, NULL);
	signal_handler_disconnect(core, "source_remove", on_source_gone, NULL);
	obs_enum_all_sources(connect_existing, NULL);
	release_now();
	pthread_mutex_destroy(&reg.mutex);
	reg.initialized = false;
}

void vf_registry_mark_dirty(void)
{
	os_atomic_set_bool(&reg.dirty, true);
}

void vf_registry_tick(void)
{
	if (os_atomic_exchange_bool(&reg.dirty, false))
		rebuild();
}

uint64_t vf_registry_generation(void)
{
	return reg.generation;
}

static const struct reg_entry *find_entry(obs_source_t *source)
{
	size_t lo = 0, hi = reg.nentries;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if ((uintptr_t)reg.entries[mid].source < (uintptr_t)source)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < reg.nentries && reg.entries[lo].source == source ? &reg.entries[lo] : NULL;
}

size_t vf_registry_get(obs_source_t *source, const struct vf_reg_copy **out)
{
	const struct reg_entry *e = find_entry(source);
	*out = e ? &reg.copies[e->first] : NULL;
	return e ? e->count : 0;
}

bool vf_reg_copy_shown(const struct vf_reg_copy *c)
{
	return vf_copy_shown(obs_sceneitem_visible(c->item), c->group != NULL,
			     c->group && obs_sceneitem_visible(c->group));
}

void vf_registry_in_scenes(obs_source_t *source, const char *a, const char *b, bool *in_a, bool *in_b)
{
	*in_a = *in_b = false;
	pthread_mutex_lock(&reg.mutex);
	const struct reg_entry *e = find_entry(source);
	for (size_t i = 0; e && i < e->count; i++) {
		const struct vf_reg_copy *c = &reg.copies[e->first + i];
		if (!vf_reg_copy_shown(c))
			continue; /* hidden there: Move shows it appearing, nothing animates from that scene */
		*in_a = *in_a || (a[0] && strcmp(c->scene_uuid, a) == 0);
		*in_b = *in_b || (b[0] && strcmp(c->scene_uuid, b) == 0);
	}
	pthread_mutex_unlock(&reg.mutex);
}

static bool item_uses_texture(obs_sceneitem_t *item);

/* UI thread: the scenes in the order of the OBS Scenes dock. */
static void sort_like_dock(char (*ids)[VF_UUID_LEN], size_t n)
{
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	const char **order = bmalloc(sizeof(const char *) * (scenes.sources.num + 1));
	for (size_t i = 0; i < scenes.sources.num; i++)
		order[i] = obs_source_get_uuid(scenes.sources.array[i]);
	vf_order_like(&ids[0][0], n, VF_UUID_LEN, order, scenes.sources.num);
	bfree(order);
	obs_frontend_source_list_free(&scenes);
}

static size_t collect_uuids(obs_source_t *source, char (*out)[VF_UUID_LEN], size_t max, bool texture_only)
{
	size_t n = 0;
	pthread_mutex_lock(&reg.mutex);
	const struct reg_entry *e = find_entry(source);
	for (size_t i = 0; e && i < e->count && n < max; i++) {
		const char *u = reg.copies[e->first + i].scene_uuid;
		if (texture_only && !item_uses_texture(reg.copies[e->first + i].item))
			continue;
		bool seen = false;
		for (size_t k = 0; k < n && !seen; k++)
			seen = strcmp(out[k], u) == 0;
		if (!seen)
			snprintf(out[n++], VF_UUID_LEN, "%s", u);
	}
	pthread_mutex_unlock(&reg.mutex);
	sort_like_dock(out, n);
	return n;
}

size_t vf_registry_scene_uuids(obs_source_t *source, char (*out)[VF_UUID_LEN], size_t max)
{
	return collect_uuids(source, out, max, false);
}

size_t vf_registry_texture_scenes(obs_source_t *source, char (*out)[VF_UUID_LEN], size_t max)
{
	return collect_uuids(source, out, max, true);
}

static bool item_uses_texture(obs_sceneitem_t *item)
{
	struct obs_sceneitem_crop k;
	obs_sceneitem_get_crop(item, &k);
	obs_source_t *src = obs_sceneitem_get_source(item);
	return k.left || k.top || k.right || k.bottom || obs_sceneitem_get_bounds_crop(item) ||
	       obs_sceneitem_get_scale_filter(item) != OBS_SCALE_DISABLE ||
	       obs_sceneitem_get_blending_mode(item) != OBS_BLEND_NORMAL ||
	       obs_sceneitem_get_blending_method(item) == OBS_BLEND_METHOD_SRGB_OFF ||
	       (obs_source_get_type(src) == OBS_SOURCE_TYPE_SCENE && !obs_sceneitem_is_group(item));
}

void vf_reg_copy_fill(const struct vf_reg_copy *c, uint32_t src_w, uint32_t src_h, int rank, struct vf_copy *out)
{
	struct matrix4 dt;
	obs_sceneitem_get_draw_transform(c->item, &dt);
	if (c->group) {
		struct matrix4 g, composed;
		obs_sceneitem_get_draw_transform(c->group, &g);
		matrix4_mul(&composed, &dt, &g);
		dt = composed;
	}
	memcpy(out->dt, &dt, sizeof(out->dt));

	struct obs_sceneitem_crop k;
	obs_sceneitem_get_crop(c->item, &k);
	out->crop_l = k.left;
	out->crop_t = k.top;
	out->crop_r = k.right;
	out->crop_b = k.bottom;
	out->texpath = item_uses_texture(c->item);
	out->rank = rank;
	out->hidden = !vf_reg_copy_shown(c);
	(void)src_w;
	(void)src_h;
}
