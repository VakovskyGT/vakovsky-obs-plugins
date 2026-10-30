/* Browser ReSize: page size per scene; during a transition the frame follows it and one checked browser resize is in
 * flight at a time. */
#include <obs-module.h>
#include <plugin-support.h>
#include <obs-frontend-api.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>
#include <graphics/vec4.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vf-about.h"
#include "copy-registry.h"
#include "scene-anim.h"
#include "profile-prune.h"
#include "vf-size.h"
#include "vf-resize.h"
#include "vf-fade.h"
#include "vf-css.h"

#define S_SMOOTH "smooth"
#define S_ANCHOR "anchor"
#define S_EDIT_SCENE "edit_scene"
#define S_FIXED_HEIGHT "fixed_height"
#define S_REDRAW "redraw"

#define BLACKOUT_MAX_NS (10000ULL * 1000000ULL) /* never freeze the picture longer than this */
#define SNAP_WAIT_NS (100ULL * 1000000ULL)     /* resize anyway when nothing renders the source to snapshot it */
/* Fixed timings, tuned on the test chat: the browser needs a frame or two after a resize before its picture is good */
#define HOLD_NS (250ULL * 1000000ULL)     /* a resize outside a switch: the last good frame stays this long */
#define END_HOLD_NS (100ULL * 1000000ULL) /* after a stepped switch the last step stays this long */
#define FADE_NS (100ULL * 1000000ULL)     /* then it fades into the live page */
#define STEP_TIMEOUT_NS (250ULL * 1000000ULL)
#define STEP_RETRIES 3u /* stuck steps in a row asked again at a new size before the move gives up its steps */
#define SPILL_PX 4u /* clear margin right and below a capture: the old, bigger picture after a shrink runs into it */

enum view { VIEW_NONE, VIEW_LIVE, VIEW_SNAP, VIEW_HIDDEN, VIEW_FADE };

struct size_entry {
	char uuid[VF_UUID_LEN];
	long long w, h;
};

struct size_image {
	gs_texrender_t *tr;
	uint32_t w, h;   /* valid pixels; the allocated texture may be larger */
	uint32_t margin; /* clear pixels drawn past w and h */
};

struct size_layout {
	bool stretch;
	float x, y;
};

struct vf_size {
	obs_source_t *self;
	gs_effect_t *effect;
	gs_eparam_t *p_image, *p_uv_scale, *p_opacity, *p_image2, *p_amount, *p_scale_a, *p_scale_b, *p_anchor;
	gs_eparam_t *p_sample_a, *p_sample_b, *p_spill, *p_edge;

	pthread_mutex_t lock;
	DARRAY(struct size_entry) sizes;
	uint64_t last_editor_ns; /* UI thread: a dialog asks for properties again while it is open */
	bool force;
	bool fixed_height, disabled;
	uint32_t working_height;
	bool smooth;              /* off: the size changes at once, as on a Cut */
	bool redraw, redraw_sent; /* the redraw block in the browser's CSS: wanted, and last asked for */
	bool stretch; /* while the size changes the page is stretched to the frame, else drawn 1:1 at the anchor */
	float anchor_x, anchor_y;
	uint64_t hold_ns; /* how long the last good frame stays once the page has the new size */
	uint64_t fade_ns; /* the last good frame fades out over the redrawn page */

	/* switch state (tick) */
	uint64_t seen_start;
	struct vf_size_anim anim;
	bool animating;
	bool frozen; /* leaving the program mid-switch: the frame stays at shown_w x shown_h (Move took that size) */
	float shown_w, shown_h, mix;
	bool dissolve, dissolve_ready;
	uint64_t dissolve_until;
	char fade_scene_from[VF_UUID_LEN], fade_scene_to[VF_UUID_LEN];
	bool mid_sent; /* the final resize was asked for */

	struct vf_resize resize;
	uint64_t epoch;
	bool finish_early;
	uint64_t step_shown_at;

	/* page resize: the last good frame is kept first, then the resize */
	bool need_snap;          /* the next render takes the snapshot */
	bool refresh_live;       /* an idle snapshot must not replay content from the previous transition */
	bool snap_done, snap_ok; /* snap_ok: the snapshot holds the current view */
	bool resize_pending;
	long long resize_w, resize_h;
	uint64_t resize_asked;

	/* a resize outside a switch: show the snapshot until the page has redrawn */
	bool blackout;
	bool hide; /* appearing: nothing to show until the page has redrawn at its new size */
	long long keep_w, keep_h;
	uint64_t reached_at, blackout_until;
	bool live_good;     /* a checked candidate matches the final size */
	uint64_t fade_from; /* the page has redrawn: the last good frame fades out from here; 0 = no fade */

	volatile long box_w, box_h; /* frame size; 0 = the source's own size */

	/* render thread */
	struct size_image live, snap, snap_prev, pend;
	gs_texrender_t *check;
	gs_stagesurf_t *stage;
	bool check_pending;
	struct vf_resize_tag check_tag;
	uint64_t check_frame, processed_frame;
	bool prev_ok;

	enum view last_view;
	uint64_t last_frame;
};

struct apply_task {
	obs_weak_source_t *target, *filter;
	struct vf_resize_tag tag;
};

static const char *vs_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BrowserReSize");
}

static void key_for(char *buf, size_t n, char axis, const char *uuid)
{
	/* uuids fit; a cut key only misses the stored size */
	if (snprintf(buf, n, "%c_%s", axis, uuid) >= (int)n)
		buf[n - 1] = '\0';
}

struct redraw_task {
	obs_weak_source_t *parent;
	bool on;
};

/* UI thread: adds or cuts the redraw block in the browser source's custom CSS (the page reloads once); the rest of the
 * CSS stays as the user wrote it */
static void redraw_apply(void *param)
{
	struct redraw_task *t = param;
	obs_source_t *parent = obs_weak_source_get_source(t->parent);
	if (parent && !obs_source_removed(parent) &&
	    strcmp(obs_source_get_unversioned_id(parent), "browser_source") == 0) {
		obs_data_t *settings = obs_source_get_settings(parent);
		char *css = vf_css_set_redraw(obs_data_get_string(settings, "css"), t->on);
		if (css) {
			obs_data_t *change = obs_data_create();
			obs_data_set_string(change, "css", css);
			obs_source_update(parent, change);
			obs_data_release(change);
			free(css);
		}
		obs_data_release(settings);
	}
	obs_source_release(parent);
	obs_weak_source_release(t->parent);
	bfree(t);
}

/* Any thread: update runs on the graphics thread, where another source's settings must not be written. */
static void queue_redraw(obs_source_t *parent, bool on)
{
	struct redraw_task *t = bzalloc(sizeof(*t));
	t->parent = obs_source_get_weak_source(parent);
	t->on = on;
	obs_queue_task(OBS_TASK_UI, redraw_apply, t, false);
}

static void vs_update(void *data, obs_data_t *settings)
{
	struct vf_size *f = data;
	pthread_mutex_lock(&f->lock);
	f->sizes.num = 0;
	for (obs_data_item_t *it = obs_data_first(settings); it; obs_data_item_next(&it)) {
		struct size_entry e = {0};
		if (!vf_size_profile_key(obs_data_item_get_name(it), sizeof(e.uuid), e.uuid))
			continue;
		bool seen = false;
		for (size_t i = 0; i < f->sizes.num && !seen; i++)
			seen = strcmp(f->sizes.array[i].uuid, e.uuid) == 0;
		if (seen)
			continue;
		char k[VF_UUID_LEN + 2];
		key_for(k, sizeof(k), 'w', e.uuid);
		e.w = obs_data_get_int(settings, k);
		key_for(k, sizeof(k), 'h', e.uuid);
		e.h = obs_data_get_int(settings, k);
		if (e.w > 0 || e.h > 0)
			da_push_back(f->sizes, &e);
	}
	f->fixed_height = obs_data_get_bool(settings, S_FIXED_HEIGHT);
	f->smooth = obs_data_get_bool(settings, S_SMOOTH);
	f->redraw = obs_data_get_bool(settings, S_REDRAW);
	/* no parent yet while the filter is created: vs_filter_add asks then */
	obs_source_t *parent = obs_filter_get_parent(f->self);
	if (parent && f->redraw != f->redraw_sent) {
		f->redraw_sent = f->redraw;
		queue_redraw(parent, f->redraw);
	}
	f->stretch = vf_size_anchor((int)obs_data_get_int(settings, S_ANCHOR), &f->anchor_x, &f->anchor_y);
	f->fade_ns = FADE_NS;
	f->force = true; /* apply the program scene's size right away */
	pthread_mutex_unlock(&f->lock);
}

/* Wait for a different video frame, not a wall-clock interval that can miss the next tick by microseconds. */
static void reset_resize(struct vf_size *f)
{
	vf_resize_reset(&f->resize, ++f->epoch, 0);
	f->resize.latency_ns = obs_get_frame_interval_ns() * 2;
}

static void *vs_create(obs_data_t *settings, obs_source_t *self)
{
	struct vf_size *f = bzalloc(sizeof(*f));
	f->self = self;
	pthread_mutex_init(&f->lock, NULL);
	da_init(f->sizes);

	char *path = obs_module_file("size.effect");
	char *err = NULL;
	obs_enter_graphics();
	f->effect = path ? gs_effect_create_from_file(path, &err) : NULL;
	if (f->effect) {
		f->p_image = gs_effect_get_param_by_name(f->effect, "image");
		f->p_uv_scale = gs_effect_get_param_by_name(f->effect, "uv_scale");
		f->p_opacity = gs_effect_get_param_by_name(f->effect, "opacity");
		f->p_image2 = gs_effect_get_param_by_name(f->effect, "image2");
		f->p_amount = gs_effect_get_param_by_name(f->effect, "amount");
		f->p_scale_a = gs_effect_get_param_by_name(f->effect, "scale_a");
		f->p_scale_b = gs_effect_get_param_by_name(f->effect, "scale_b");
		f->p_anchor = gs_effect_get_param_by_name(f->effect, "anchor");
		f->p_sample_a = gs_effect_get_param_by_name(f->effect, "sample_a");
		f->p_sample_b = gs_effect_get_param_by_name(f->effect, "sample_b");
		f->p_spill = gs_effect_get_param_by_name(f->effect, "spill");
		f->p_edge = gs_effect_get_param_by_name(f->effect, "edge");
	}
	f->live.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->snap.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->snap_prev.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->pend.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->check = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->stage = gs_stagesurface_create(1, 1, GS_RGBA);
	obs_leave_graphics();
	if (!f->effect)
		obs_log(LOG_ERROR, "size.effect failed: %s", err ? err : "not found");
	bfree(err);
	bfree(path);

	reset_resize(f);
	vs_update(f, settings);
	return f;
}

static void vs_destroy(void *data)
{
	struct vf_size *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->live.tr);
	gs_texrender_destroy(f->snap.tr);
	gs_texrender_destroy(f->snap_prev.tr);
	gs_texrender_destroy(f->pend.tr);
	gs_texrender_destroy(f->check);
	gs_stagesurface_destroy(f->stage);
	obs_leave_graphics();
	da_free(f->sizes);
	pthread_mutex_destroy(&f->lock);
	bfree(f);
}

/* UI thread: a source's update is what its properties dialog does, so the browser resizes without a reload. The real
 * size is checked too: equal settings do not mean the page has that size. */
static void apply_on_ui(void *param)
{
	struct apply_task *t = param;
	obs_source_t *filter = obs_weak_source_get_source(t->filter);
	obs_source_t *src = obs_weak_source_get_source(t->target);
	struct vf_size *f = filter ? obs_obj_get_data(filter) : NULL;
	if (f && src) {
		pthread_mutex_lock(&f->lock);
		/* A later transition can replace a queued request before the UI gets to it. */
		if (obs_filter_get_parent(filter) == src && vf_resize_current(&f->resize, t->tag) &&
		    f->resize.phase == VF_RESIZE_QUEUED) {
			obs_data_t *cur = obs_source_get_settings(src);
			if (vf_size_needs_update(obs_data_get_int(cur, "width"), obs_data_get_int(cur, "height"),
						 obs_source_get_base_width(src), obs_source_get_base_height(src),
						 t->tag.w, t->tag.h)) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "width", t->tag.w);
				obs_data_set_int(d, "height", t->tag.h);
				obs_source_update(src, d);
				obs_data_release(d);
			}
			obs_data_release(cur);
			vf_resize_applied(&f->resize, t->tag, os_gettime_ns(), obs_get_video_frame_time());
		}
		pthread_mutex_unlock(&f->lock);
	}
	obs_source_release(src);
	obs_source_release(filter);
	obs_weak_source_release(t->target);
	obs_weak_source_release(t->filter);
	bfree(t);
}

static void queue_resize(struct vf_size *f, obs_source_t *parent, struct vf_resize_tag tag)
{
	struct apply_task *task = bzalloc(sizeof(*task));
	task->target = obs_source_get_weak_source(parent);
	task->filter = obs_source_get_weak_source(f->self);
	task->tag = tag;
	obs_queue_task(OBS_TASK_UI, apply_on_ui, task, false);
}

/* Sizes of scenes that no longer exist (profile-prune.c, at collection load). */
void vf_size_prune(obs_source_t *filter, const char *const *known, size_t n)
{
	struct vf_size *f = obs_obj_get_data(filter);
	if (!f)
		return;
	obs_data_t *settings = obs_source_get_settings(filter);
	DARRAY(char *) drop;
	da_init(drop);
	pthread_mutex_lock(&f->lock); /* vs_update walks the same settings on the graphics thread */
	for (obs_data_item_t *it = obs_data_first(settings); it; obs_data_item_next(&it)) {
		const char *name = obs_data_item_get_name(it);
		if (vf_size_stale_key(name, known, n)) {
			char *copy = bstrdup(name);
			da_push_back(drop, &copy);
		}
	}
	for (size_t i = 0; i < drop.num; i++) {
		obs_data_erase(settings, drop.array[i]);
		bfree(drop.array[i]);
	}
	pthread_mutex_unlock(&f->lock);
	if (drop.num)
		obs_source_update(filter, NULL);
	da_free(drop);
	obs_data_release(settings);
}

/* The picture this filter gets: the page after any filters below it (a crop). */
static uint32_t own_size(struct vf_size *f, bool height)
{
	obs_source_t *target = obs_filter_get_target(f->self);
	return target ? (height ? obs_source_get_base_height(target) : obs_source_get_base_width(target)) : 0;
}

/* The browser's own viewport: what scene profiles, resize requests and the capture button are about. */
static uint32_t page_size(struct vf_size *f, bool height)
{
	obs_source_t *parent = obs_filter_get_parent(f->self);
	return parent ? (height ? obs_source_get_base_height(parent) : obs_source_get_base_width(parent)) : 0;
}

/* A page-space length as the picture this filter gets. */
static long to_image(struct vf_size *f, long page_len, bool height)
{
	return vf_size_to_image(page_len, page_size(f, height), own_size(f, height));
}

/* The scene's page size, a 0 field resolved to the page's current value; NULL when the scene has none. */
static const struct size_entry *find_size(struct vf_size *f, const char *uuid, struct size_entry *out)
{
	for (size_t i = 0; uuid[0] && i < f->sizes.num; i++) {
		const struct size_entry *e = &f->sizes.array[i];
		if (strcmp(e->uuid, uuid) == 0) {
			*out = *e;
			return vf_size_resolve(e->w, e->h, page_size(f, false), page_size(f, true), &out->w, &out->h)
				       ? out
				       : NULL;
		}
	}
	return NULL;
}

static uint32_t page_height(struct vf_size *f, uint32_t h)
{
	return vf_size_page_height(f->fixed_height, f->animating, h, f->working_height);
}

/* Caller holds f->lock. The page goes to w x h after the next render has kept the current view. */
static void ask_resize(struct vf_size *f, long long w, long long h, uint64_t now)
{
	f->need_snap = true;
	f->snap_done = false;
	f->resize_pending = true;
	f->resize_w = w;
	f->resize_h = h;
	f->resize_asked = now;
	f->hold_ns = HOLD_NS;
	f->live_good = false;
	f->fade_from = 0;
}

static void vs_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct vf_size *f = data;
	obs_source_t *parent = obs_filter_get_parent(f->self);
	const uint64_t now = os_gettime_ns();
	bool send = false, retry = false;
	struct vf_resize_tag send_tag = {0};
	long long send_w = 0, send_h = 0;

	pthread_mutex_lock(&f->lock);
	if (!parent || !obs_source_enabled(f->self)) {
		if (!parent)
			reset_resize(f);
		if (parent && !f->disabled && (f->animating || f->resize_pending || f->fixed_height)) {
			send_w = f->resize_pending ? f->resize_w : f->anim.to_w;
			send_h = f->resize_pending ? f->resize_h : f->anim.to_h;
			if (f->fixed_height) {
				struct vf_anim_state current;
				vf_anim_get(&current);
				struct size_entry found;
				const struct size_entry *e = find_size(f, current.program_uuid, &found);
				if (e) {
					send_w = e->w;
					send_h = e->h;
				}
			}
			reset_resize(f);
			send = vf_resize_issue(&f->resize, (uint32_t)send_w, (uint32_t)send_h, now, true);
			send_tag = f->resize.tag;
		}
		f->disabled = true;
		f->dissolve = false;
		f->animating = f->frozen = f->resize_pending = f->need_snap = f->blackout = f->hide = false;
		pthread_mutex_unlock(&f->lock);
		os_atomic_set_long(&f->box_w, 0);
		os_atomic_set_long(&f->box_h, 0);
		if (send)
			queue_resize(f, parent, send_tag);
		return;
	}

	f->disabled = false;
	struct vf_anim_state as;
	vf_anim_get(&as);

	if (as.start_ns && as.start_ns != f->seen_start) {
		f->seen_start = as.start_ns;
		struct size_entry to_found, from_found;
		const struct size_entry *to = find_size(f, as.program_uuid, &to_found);
		bool in_from, in_program;
		vf_registry_in_scenes(parent, as.from_uuid, as.program_uuid, &in_from, &in_program);
		const enum vf_size_switch kind =
			vf_size_switch_kind(in_from, in_program, to != NULL, as.transition_ms, f->smooth);
		const bool busy = f->animating || f->frozen || f->blackout ||
				  (f->fade_from && vf_size_fade_alpha(f->fade_from, now, f->fade_ns) > 0.0f);
		if (kind != VF_SW_NONE) {
			f->frozen = false;
			f->dissolve = f->dissolve_ready = false;
			reset_resize(f);
			f->finish_early = false;
			f->fade_from = 0;
			f->live_good = false;
		}
		if (kind == VF_SW_KEEP && f->animating) {
			/* leaving: the frame stays as it is now; a resize not yet sent is dropped, the page is not
			 * on screen there */
			f->animating = false;
			f->frozen = true;
			if (f->resize_pending)
				f->resize_pending = f->need_snap = f->blackout = false;
		} else if (kind == VF_SW_JUMP || kind == VF_SW_ANIMATE) {
			const struct size_entry *from = find_size(f, as.from_uuid, &from_found);
			long long fw, fh;
			long shown_w = os_atomic_load_long(&f->box_w), shown_h = os_atomic_load_long(&f->box_h);
			/* explicit float: on Windows long is 32-bit, so the mix with uint32_t is unsigned long (C4244) */
			const float box_w = shown_w > 0 ? (float)shown_w : (float)page_size(f, false);
			const float box_h = shown_h > 0 ? (float)shown_h : (float)page_size(f, true);
			vf_size_pick_from(busy, box_w, box_h, from != NULL, from ? from->w : 0, from ? from->h : 0,
					  page_size(f, false), page_size(f, true), &fw, &fh);
			f->anim = (struct vf_size_anim){as.start_ns,    as.transition_ms, fw, fh, to->w, to->h,
							as.move_easing, as.move_easing_fn};
			f->animating = kind == VF_SW_ANIMATE && fw > 0 && fh > 0;
			f->dissolve = !f->smooth && as.transition_ms > 0 && in_from && in_program;
			if (f->dissolve) {
				f->dissolve_until = as.start_ns + (uint64_t)as.transition_ms * 1000000ULL;
				memcpy(f->fade_scene_from, as.from_uuid, VF_UUID_LEN);
				memcpy(f->fade_scene_to, as.program_uuid, VF_UUID_LEN);
			}
			f->working_height = page_size(f, true);
			if ((uint32_t)to->h > f->working_height)
				f->working_height = (uint32_t)to->h;
			f->blackout = !f->animating;
			/* appearing: the new size at once, Move takes the item's size on its first frame; a Cut keeps
			 * the old frame until the page has redrawn */
			f->keep_w = !in_from ? to->w : fw > 0 ? fw : page_size(f, false);
			f->keep_h = !in_from ? to->h : fh > 0 ? fh : page_size(f, true);
			f->mid_sent = false;

			f->hide = !in_from;
			if (!in_from && page_size(f, false) == (uint32_t)to->w && page_size(f, true) == (uint32_t)to->h)
				f->blackout = f->hide = false; /* appearing at the size it already has */
			else if (!f->animating)
				ask_resize(f, to->w, to->h, now);
			if (f->hide) { /* the old page is never on screen here: no snapshot to wait for */
				f->need_snap = false;
				f->snap_done = true;
			} else if (f->animating) {
				f->resize_pending = false; /* the scheduler owns the intermediate resizes */
				f->need_snap = !busy;
				f->refresh_live = !busy;
				f->snap_done = busy;
			}
		}
		/* VF_SW_NONE: no size on the new scene, a running switch just finishes towards its own target */
		f->force = false;
	}
	bool unused, in_program = false;
	if (f->force && as.program_uuid[0])
		vf_registry_in_scenes(parent, as.program_uuid, as.program_uuid, &unused, &in_program);
	/* waits until the program scene is known and holds the source (at load the registry fills in a few ticks);
	 * a size kept for a scene the source has left is never applied */
	if (f->force && in_program) {
		f->force = false;
		struct size_entry found;
		const struct size_entry *e = find_size(f, as.program_uuid, &found);
		if (e && !f->animating && !f->frozen &&
		    (e->w != page_size(f, false) || (uint32_t)e->h != page_size(f, true))) {
			reset_resize(f);
			f->blackout = true;
			long visible_w = os_atomic_load_long(&f->box_w), visible_h = os_atomic_load_long(&f->box_h);
			f->keep_w = visible_w > 0 ? visible_w : page_size(f, false);
			f->keep_h = visible_h > 0 ? visible_h : page_size(f, true);
			ask_resize(f, e->w, e->h, now);
		}
	}

	if (f->resize_pending && (f->snap_done || now - f->resize_asked >= SNAP_WAIT_NS)) {
		f->resize_pending = false;
		f->need_snap = false;
		send = true;
		send_w = f->resize_w;
		send_h = f->resize_h;
		f->reached_at = 0;
		f->blackout_until = now + BLACKOUT_MAX_NS;
	}

	long bw = 0, bh = 0;
	if (f->animating) {
		float fw, fh;
		long long pw, ph;
		f->animating = vf_size_anim_at(&f->anim, now, &fw, &fh, &pw, &ph, &f->mix);
		f->shown_w = fw;
		f->shown_h = fh;
		if (f->animating) {
			if (vf_resize_expired(&f->resize, now, STEP_TIMEOUT_NS) && !f->finish_early) {
				/* one bad step must not cost the rest of the move: ask again where the move is now */
				const uint32_t rw = (uint32_t)fmaxf(1.0f, roundf(fw));
				const uint32_t rh = page_height(f, (uint32_t)fmaxf(1.0f, roundf(fh)));
				const bool same = rw == f->resize.tag.w && rh == f->resize.tag.h;
				if (!same && vf_resize_retry(&f->resize, rw, rh, now, STEP_RETRIES)) {
					retry = true;
				} else if (!same) {
					f->finish_early = true;
				}
			}
			if (f->finish_early) {
				send = true;
				send_w = f->anim.to_w;
				send_h = page_height(f, (uint32_t)f->anim.to_h);
			}
		}
		if (!f->mid_sent && !f->animating) {
			/* the switch is over: the last step stays until the page has redrawn at the new size */
			f->mid_sent = true;
			f->blackout = true;
			f->keep_w = f->anim.to_w;
			f->keep_h = f->anim.to_h;
			ask_resize(f, f->anim.to_w, f->anim.to_h, now);
			f->hold_ns = END_HOLD_NS;
			/* hold the last checked step snapshot, not a fresh one: the browser may be mid-redraw now */
			f->need_snap = false;
			f->snap_done = true;
		}
		if (f->animating) {
			bw = lroundf(fw) > 0 ? lroundf(fw) : 1;
			bh = lroundf(fh) > 0 ? lroundf(fh) : 1;
		}
	}
	if (f->frozen) {
		bw = lroundf(f->shown_w) > 0 ? lroundf(f->shown_w) : 1;
		bh = lroundf(f->shown_h) > 0 ? lroundf(f->shown_h) : 1;
	}
	if (f->blackout) {
		if (!f->resize_pending && page_size(f, false) == (uint32_t)f->resize_w &&
		    page_size(f, true) == (uint32_t)f->resize_h && !f->reached_at)
			f->reached_at = now;
		const bool held = f->reached_at && now - f->reached_at >= f->hold_ns;
		if (!f->resize_pending && ((held && f->live_good) || now >= f->blackout_until)) {
			/* A page with legitimate black edges can fail the heuristic indefinitely. */
			if (!f->live_good)
				reset_resize(f);
			f->fade_from = f->hide ? 0 : now; /* hidden: no last frame was on screen */
			f->blackout = false;
			f->hide = false;
		}
		if (f->blackout && !bw) {
			bw = (long)f->keep_w;
			bh = (long)f->keep_h;
		}
	}

	if (f->dissolve) {
		if (now >= f->dissolve_until)
			f->dissolve = false;
		else {
			bw = (long)f->anim.to_w;
			bh = (long)f->anim.to_h;
		}
	}

	if (send) {
		send = vf_resize_issue(&f->resize, (uint32_t)send_w, (uint32_t)send_h, now, true);
		send_tag = f->resize.tag;
		if (send) {
			f->live_good = false;
			f->resize.settle_ns = vf_size_settle_ns(f->dissolve, f->fixed_height, (uint32_t)send_h,
								page_size(f, true), obs_get_frame_interval_ns());
		}
	}
	if (retry && !send && f->resize.phase == VF_RESIZE_QUEUED) {
		/* vf_resize_retry has already issued the request */
		send = true;
		send_tag = f->resize.tag;
		f->live_good = false;
		f->resize.settle_ns = vf_size_settle_ns(false, f->fixed_height, send_tag.h, page_size(f, true),
							obs_get_frame_interval_ns());
	}
	if (f->resize.phase == VF_RESIZE_IDLE && f->resize.have_accepted &&
	    f->resize.accepted_w == (uint32_t)f->resize_w && f->resize.accepted_h == (uint32_t)f->resize_h)
		f->live_good = true;
	pthread_mutex_unlock(&f->lock);

	os_atomic_set_long(&f->box_w, bw);
	os_atomic_set_long(&f->box_h, bh);
	if (send)
		queue_resize(f, parent, send_tag);
}

/* The scene scope filter (scene-scope.c) names each child scene of a transition before OBS computes its items'
 * transforms. */
static uint32_t fade_copy_size(struct vf_size *f, bool height)
{
	pthread_mutex_lock(&f->lock);
	enum vf_size_fade_role role =
		vf_size_fade_role(f->dissolve, vf_fade_scene_get(), f->fade_scene_from, f->fade_scene_to);
	uint32_t size = role == VF_SIZE_FADE_OLD   ? (uint32_t)(height ? f->anim.from_h : f->anim.from_w)
			: role == VF_SIZE_FADE_NEW ? (uint32_t)(height ? f->anim.to_h : f->anim.to_w)
						   : 0;
	pthread_mutex_unlock(&f->lock);
	return size ? (uint32_t)to_image(f, (long)size, height) : 0;
}

static uint32_t vs_get_width(void *data)
{
	struct vf_size *f = data;
	uint32_t copy = fade_copy_size(f, false);
	if (copy)
		return copy;
	long v = os_atomic_load_long(&f->box_w);
	return v > 0 ? (uint32_t)to_image(f, v, false) : own_size(f, false);
}

static uint32_t vs_get_height(void *data)
{
	struct vf_size *f = data;
	uint32_t copy = fade_copy_size(f, true);
	if (copy)
		return copy;
	long v = os_atomic_load_long(&f->box_h);
	return v > 0 ? (uint32_t)to_image(f, v, true) : own_size(f, true);
}

/* Allocate in buckets, but render and sample only the valid source pixels. */
static bool render_source(struct vf_size *f, struct size_image *image, uint32_t w, uint32_t h, uint32_t margin)
{
	obs_source_t *target = obs_filter_get_target(f->self);
	obs_source_t *parent = obs_filter_get_parent(f->self);
	if (!target || !parent || !w || !h)
		return false;
	uint32_t aw = (w + margin + 63u) & ~63u, ah = (h + margin + 63u) & ~63u;
	gs_texrender_reset(image->tr);
	if (!gs_texrender_begin(image->tr, aw, ah))
		return false;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_set_viewport(0, 0, (int)(w + margin), (int)(h + margin));
	gs_ortho(0.0f, (float)(w + margin), 0.0f, (float)(h + margin), -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	const uint32_t flags = obs_source_get_output_flags(parent);
	if (target == parent && !(flags & OBS_SOURCE_CUSTOM_DRAW) && !(flags & OBS_SOURCE_ASYNC))
		obs_source_default_render(target);
	else
		obs_source_video_render(target);
	gs_blend_state_pop();
	gs_texrender_end(image->tr);
	image->w = w;
	image->h = h;
	image->margin = margin;
	return true;
}

static void draw_snap(struct vf_size *f, const struct size_image *a, const struct size_image *b, float amount,
		      uint32_t w, uint32_t h, float opacity, struct size_layout layout)
{
	gs_texture_t *ta = gs_texrender_get_texture(a->tr), *tb = gs_texrender_get_texture(b->tr);
	if (!ta || !tb || !a->w || !a->h || !b->w || !b->h)
		return;
	struct vec2 sa, sb, an, va, vb;
	vec2_set(&sa, layout.stretch ? 1.0f : (float)w / a->w, layout.stretch ? 1.0f : (float)h / a->h);
	vec2_set(&sb, layout.stretch ? 1.0f : (float)w / b->w, layout.stretch ? 1.0f : (float)h / b->h);
	vec2_set(&va, (float)a->w / gs_texture_get_width(ta), (float)a->h / gs_texture_get_height(ta));
	vec2_set(&vb, (float)b->w / gs_texture_get_width(tb), (float)b->h / gs_texture_get_height(tb));
	vec2_set(&an, layout.x, layout.y);
	gs_effect_set_vec2(f->p_scale_a, &sa);
	gs_effect_set_vec2(f->p_scale_b, &sb);
	gs_effect_set_vec2(f->p_sample_a, &va);
	gs_effect_set_vec2(f->p_sample_b, &vb);
	gs_effect_set_vec2(f->p_anchor, &an);
	gs_effect_set_texture(f->p_image, ta);
	gs_effect_set_texture(f->p_image2, tb);
	gs_effect_set_float(f->p_amount, amount);
	gs_effect_set_float(f->p_opacity, opacity);
	while (gs_effect_loop(f->effect, "Mix"))
		gs_draw_sprite(tb, 0, w, h);
}

/* Only a 1x1 result crosses back to the CPU, on a later video frame. */
static bool run_check(struct vf_size *f, const struct size_image *image)
{
	gs_texture_t *tex = gs_texrender_get_texture(image->tr);
	if (!tex || !f->stage)
		return false;
	gs_texrender_reset(f->check);
	if (!gs_texrender_begin(f->check, 1, 1))
		return false;
	gs_ortho(0.0f, 1.0f, 0.0f, 1.0f, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	struct vec2 uv;
	vec2_set(&uv, (float)image->w / gs_texture_get_width(tex), (float)image->h / gs_texture_get_height(tex));
	gs_effect_set_vec2(f->p_uv_scale, &uv);
	struct vec2 spill; /* the middle of the margin's first column and row past the valid pixels */
	vec2_set(&spill, image->margin ? ((float)image->w + 1.5f) / gs_texture_get_width(tex) : 0.0f,
		 image->margin ? ((float)image->h + 1.5f) / gs_texture_get_height(tex) : 0.0f);
	gs_effect_set_vec2(f->p_spill, &spill);
	struct vec2 edge; /* the outermost valid pixels: a strip one pixel wide at the edge is a broken frame too */
	vec2_set(&edge, 0.5f / (float)image->w, 0.5f / (float)image->h);
	gs_effect_set_vec2(f->p_edge, &edge);
	gs_effect_set_texture(f->p_image, tex);
	while (gs_effect_loop(f->effect, "Check"))
		gs_draw_sprite(tex, 0, 1, 1);
	gs_blend_state_pop();
	gs_texrender_end(f->check);
	gs_stage_texture(f->stage, gs_texrender_get_texture(f->check));
	return true;
}

/* why: the edges with a black strip (top 1, bottom 2, left 4, right 8) and 16 when the picture spilled past the frame */
static bool read_check(struct vf_size *f, unsigned *why)
{
	uint8_t *data;
	uint32_t linesize;
	*why = 0;
	if (!gs_stagesurface_map(f->stage, &data, &linesize))
		return false;
	bool good = data[0] <= 127;
	*why = (unsigned)lroundf(data[1] * 16.0f / 255.0f) | (data[2] > 127 ? 16u : 0u);
	gs_stagesurface_unmap(f->stage);
	return good;
}

static void take_snapshot(struct vf_size *f, uint32_t w, uint32_t h, bool fresh)
{
	if (fresh && f->snap_ok && (f->last_view == VIEW_SNAP || f->last_view == VIEW_FADE))
		return;
	f->snap_ok = render_source(f, &f->snap, w, h, 0);
}

static void vs_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct vf_size *f = data;
	const uint32_t sw = own_size(f, false), sh = own_size(f, true);
	const uint64_t frame = obs_get_video_frame_time(), now = os_gettime_ns();
	if (!f->effect || !sw || !sh) {
		obs_source_skip_video_filter(f->self);
		return;
	}

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	/* Preview, output and nested captures can all render this filter in one video frame. */
	if (frame != f->processed_frame) {
		f->processed_frame = frame;
		pthread_mutex_lock(&f->lock);
		bool need_snap = f->need_snap;
		bool refresh_live = f->refresh_live;
		f->need_snap = false;
		f->refresh_live = false;
		pthread_mutex_unlock(&f->lock);
		if (need_snap) {
			take_snapshot(f, sw, sh,
				      !refresh_live && frame - f->last_frame <= 3 * obs_get_frame_interval_ns());
			if (refresh_live)
				f->prev_ok = false;
			pthread_mutex_lock(&f->lock);
			f->snap_done = true;
			pthread_mutex_unlock(&f->lock);
		}

		if (f->check_pending && frame > f->check_frame) {
			unsigned why;
			bool good = read_check(f, &why);
			pthread_mutex_lock(&f->lock);
			enum vf_resize_result result = vf_resize_complete(&f->resize, f->check_tag, frame, now, good);
			if (result == VF_RESIZE_ACCEPTED) {
				struct size_image spare = f->snap_prev;
				f->snap_prev = f->snap;
				f->snap = f->pend;
				f->pend = spare;
				f->prev_ok = f->snap_ok;
				f->snap_ok = true;
				if (f->dissolve)
					f->dissolve_ready = true;
				f->step_shown_at = now;
				f->live_good = f->check_tag.w == (uint32_t)f->resize_w &&
					       f->check_tag.h == (uint32_t)f->resize_h;
			}
			UNUSED_PARAMETER(why);
			pthread_mutex_unlock(&f->lock);
			f->check_pending = false;
		}

		pthread_mutex_lock(&f->lock);
		/* requests are page sizes; a crop below this filter makes the picture smaller */
		bool sample = !f->check_pending &&
			      vf_resize_sample(&f->resize, now, frame, page_size(f, false), page_size(f, true));
		struct vf_resize_tag tag = f->resize.tag;
		pthread_mutex_unlock(&f->lock);
		if (sample) {
			if (render_source(f, &f->pend, sw, sh, SPILL_PX) && run_check(f, &f->pend)) {
				f->check_tag = tag;
				f->check_frame = frame;
				f->check_pending = true;
			} else {
				pthread_mutex_lock(&f->lock);
				/* No candidate was submitted; retry capture without another browser update. */
				if (vf_resize_current(&f->resize, tag))
					f->resize.phase = VF_RESIZE_WAIT;
				pthread_mutex_unlock(&f->lock);
			}
		}

		pthread_mutex_lock(&f->lock);
		bool issue = false;
		struct vf_resize_tag next_tag = {0};
		if (f->animating && f->snap_done && !f->finish_early && f->resize.phase == VF_RESIZE_IDLE) {
			float tw, th, tm;
			long long pw, ph;
			vf_size_anim_at(&f->anim, now + f->resize.latency_ns, &tw, &th, &pw, &ph, &tm);
			issue = vf_resize_issue(&f->resize, (uint32_t)fmaxf(1.0f, roundf(tw)),
						page_height(f, (uint32_t)fmaxf(1.0f, roundf(th))), now, false);
			next_tag = f->resize.tag;
			if (issue) {
				f->live_good = false;
				f->resize.settle_ns = vf_size_settle_ns(false, f->fixed_height, next_tag.h,
									page_size(f, true),
									obs_get_frame_interval_ns());
			}
		}
		pthread_mutex_unlock(&f->lock);
		obs_source_t *parent = obs_filter_get_parent(f->self);
		if (issue && parent)
			queue_resize(f, parent, next_tag);
	}

	pthread_mutex_lock(&f->lock);
	enum vf_size_fade_role role =
		vf_size_fade_role(f->dissolve, vf_fade_scene_get(), f->fade_scene_from, f->fade_scene_to);
	bool dissolve_ready = f->dissolve_ready;
	uint32_t dw = (uint32_t)(role == VF_SIZE_FADE_OLD ? f->anim.from_w : f->anim.to_w);
	uint32_t dh = (uint32_t)(role == VF_SIZE_FADE_OLD ? f->anim.from_h : f->anim.to_h);
	bool stepping = f->animating, animating = f->animating || f->frozen;
	bool blackout = f->blackout, hide = f->hide;
	float fade = f->fade_from ? vf_size_fade_alpha(f->fade_from, now, f->fade_ns) : 0.0f;
	struct size_layout layout = {f->stretch, f->anchor_x, f->anchor_y};
	pthread_mutex_unlock(&f->lock);
	if (role != VF_SIZE_FADE_LIVE) {
		dw = (uint32_t)to_image(f, (long)dw, false);
		dh = (uint32_t)to_image(f, (long)dh, true);
	}
	long bw = os_atomic_load_long(&f->box_w), bh = os_atomic_load_long(&f->box_h);
	if (bw > 0 && bh > 0) {
		bw = to_image(f, bw, false);
		bh = to_image(f, bh, true);
	}
	const bool have_snap = f->snap_ok && gs_texrender_get_texture(f->snap.tr);
	enum view view = VIEW_NONE;
	if (role == VF_SIZE_FADE_OLD && have_snap) {
		const struct size_image *image = dissolve_ready && f->prev_ok ? &f->snap_prev : &f->snap;
		draw_snap(f, image, image, 1.0f, dw, dh, 1.0f, layout);
		view = VIEW_SNAP;
	} else if (role == VF_SIZE_FADE_NEW) {
		if (dissolve_ready && have_snap) {
			draw_snap(f, &f->snap, &f->snap, 1.0f, dw, dh, 1.0f, layout);
			view = VIEW_SNAP;
		} else
			view = VIEW_HIDDEN;
	} else if (stepping && bw > 0 && bh > 0 && have_snap && !blackout) {
		float amount = vf_resize_blend(f->step_shown_at, now, obs_get_frame_interval_ns());
		draw_snap(f, f->prev_ok ? &f->snap_prev : &f->snap, &f->snap, f->prev_ok ? amount : 1.0f, (uint32_t)bw,
			  (uint32_t)bh, 1.0f, layout);
		view = VIEW_SNAP;
	} else if (bw > 0 && bh > 0 && blackout && hide) {
		view = VIEW_HIDDEN;
	} else if (bw > 0 && bh > 0 && blackout && have_snap) {
		draw_snap(f, &f->snap, &f->snap, 1.0f, (uint32_t)bw, (uint32_t)bh, 1.0f, layout);
		view = VIEW_SNAP;
	} else if ((animating && bw > 0 && bh > 0) || (fade > 0.0f && have_snap)) {
		uint32_t fw = bw > 0 ? (uint32_t)bw : sw, fh = bh > 0 ? (uint32_t)bh : sh;
		if (render_source(f, &f->live, sw, sh, 0)) {
			draw_snap(f, &f->live, &f->live, 1.0f, fw, fh, 1.0f, layout);
			view = VIEW_LIVE;
			if (fade > 0.0f && have_snap) {
				draw_snap(f, &f->snap, &f->snap, 1.0f, fw, fh, fade, layout);
				view = VIEW_FADE;
			}
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
	if (view == VIEW_NONE)
		obs_source_skip_video_filter(f->self);
	f->last_view = view;
	f->last_frame = frame;
}

/* Stores the source's current size as the size of the scene picked in the list (program without a pick). */
static bool capture_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct vf_size *f = data;
	obs_source_t *parent = obs_filter_get_parent(f->self);
	obs_source_t *program = obs_frontend_get_current_scene();
	if (parent && program) {
		obs_data_t *settings = obs_source_get_settings(f->self);
		const char *scene = vf_size_capture_scene(obs_data_get_string(settings, S_EDIT_SCENE),
							  obs_source_get_uuid(program));
		char kw[VF_UUID_LEN + 2], kh[VF_UUID_LEN + 2];
		key_for(kw, sizeof(kw), 'w', scene);
		key_for(kh, sizeof(kh), 'h', scene);
		pthread_mutex_lock(&f->lock); /* vs_update walks the same settings on the graphics thread */
		/* the real size: the settings can disagree with it after a properties dialog's Cancel */
		obs_data_set_int(settings, kw, page_size(f, false));
		obs_data_set_int(settings, kh, page_size(f, true));
		pthread_mutex_unlock(&f->lock);
		obs_source_update(f->self, NULL);
		obs_data_release(settings);
	}
	obs_source_release(program);
	return true;
}

static bool smooth_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	obs_property_set_visible(obs_properties_get(props, S_FIXED_HEIGHT), obs_data_get_bool(settings, S_SMOOTH));
	return true;
}

/* Shows the width and height of the scene picked in the list. */
static bool edit_scene_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	const char *want = obs_data_get_string(settings, S_EDIT_SCENE);
	for (size_t i = 0; i < obs_property_list_item_count(prop); i++) {
		const char *uuid = obs_property_list_item_string(prop, i);
		bool on = strcmp(uuid, want) == 0;
		char k[VF_UUID_LEN + 2];
		key_for(k, sizeof(k), 'w', uuid);
		obs_property_set_visible(obs_properties_get(props, k), on);
		key_for(k, sizeof(k), 'h', uuid);
		obs_property_set_visible(obs_properties_get(props, k), on);
	}
	return true;
}

/* UI thread. A newly opened dialog starts on the scene being worked on: the preview scene in Studio Mode, else
 * program. */
static bool pick_edit_scene(struct vf_size *f, char (*uuids)[VF_UUID_LEN], size_t n, bool fresh)
{
	struct vf_anim_state as;
	vf_anim_get(&as);
	obs_data_t *settings = obs_source_get_settings(f->self);
	size_t i = vf_size_edit_pick(obs_data_get_string(settings, S_EDIT_SCENE),
				     as.preview_uuid[0] ? as.preview_uuid : as.program_uuid, fresh, &uuids[0][0],
				     VF_UUID_LEN, n);
	bool changed = strcmp(obs_data_get_string(settings, S_EDIT_SCENE), uuids[i]) != 0;
	if (changed) {
		pthread_mutex_lock(&f->lock); /* vs_update walks the same settings on the graphics thread */
		obs_data_set_string(settings, S_EDIT_SCENE, uuids[i]);
		pthread_mutex_unlock(&f->lock);
	}
	obs_data_release(settings);
	return changed;
}

static void select_edit_scene(struct vf_size *f, char (*uuids)[VF_UUID_LEN], size_t n)
{
	uint64_t now = os_gettime_ns();
	bool fresh = now - f->last_editor_ns > 1500000000ULL;
	f->last_editor_ns = now;
	pick_edit_scene(f, uuids, n, fresh);
}

/* UI thread. A scene switch: an open properties dialog follows it to the scene now worked on (resize-main.c). */
void vf_size_follow_scene(obs_source_t *filter)
{
	struct vf_size *f = obs_obj_get_data(filter);
	obs_source_t *parent = f ? obs_filter_get_parent(f->self) : NULL;
	char uuids[64][VF_UUID_LEN];
	size_t n = parent ? vf_registry_scene_uuids(parent, uuids, 64) : 0;
	if (n && pick_edit_scene(f, uuids, n, true))
		obs_source_update_properties(filter);
}

static obs_properties_t *vs_properties(void *data)
{
	struct vf_size *f = data;
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *motion = obs_properties_create();
	obs_property_t *smooth = obs_properties_add_bool(motion, S_SMOOTH, obs_module_text("Size.SmoothMove"));
	obs_property_set_long_description(smooth, obs_module_text("Size.SmoothMoveTip"));
	obs_property_set_modified_callback(smooth, smooth_changed);
	obs_property_t *fixed = obs_properties_add_bool(motion, S_FIXED_HEIGHT, obs_module_text("Size.FixedHeight"));
	obs_property_set_long_description(fixed, obs_module_text("Size.FixedHeightTip"));
	obs_property_t *anchor = obs_properties_add_list(motion, S_ANCHOR, obs_module_text("Size.ContentAnchor"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	static const char *anchor_keys[10] = {"Size.Anchor.Stretch", "Size.Anchor.TL", "Size.Anchor.T",
					      "Size.Anchor.TR",      "Size.Anchor.L",  "Size.Anchor.C",
					      "Size.Anchor.R",       "Size.Anchor.BL", "Size.Anchor.B",
					      "Size.Anchor.BR"};
	for (int i = 0; i < 10; i++)
		obs_property_list_add_int(anchor, obs_module_text(anchor_keys[i]), i);
	obs_property_set_long_description(anchor, obs_module_text("Size.AnchorTip"));
	obs_property_t *redraw = obs_properties_add_bool(motion, S_REDRAW, obs_module_text("Size.Redraw"));
	obs_property_set_long_description(redraw, obs_module_text("Size.RedrawTip"));
	obs_properties_add_group(props, "transition_group", obs_module_text("Size.Transition"), OBS_GROUP_NORMAL,
				 motion);
	obs_properties_t *sizes = obs_properties_create();
	/* the per-scene part: what the sizes mean, the capture button, then the scenes */
	obs_properties_add_text(sizes, "hint", obs_module_text("Size.Hint"), OBS_TEXT_INFO);
	obs_properties_add_button2(sizes, "capture", obs_module_text("Size.Capture"), capture_clicked, NULL);

	/* one scene at a time, picked in a list as in Ultimate Frame; the sizes stay in their per-scene keys */
	obs_source_t *parent = f ? obs_filter_get_parent(f->self) : NULL;
	char uuids[64][VF_UUID_LEN];
	size_t n = parent ? vf_registry_scene_uuids(parent, uuids, 64) : 0;
	obs_property_t *list = obs_properties_add_list(sizes, S_EDIT_SCENE, obs_module_text("Size.Scene"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < n; i++) {
		obs_source_t *scene = obs_get_source_by_uuid(uuids[i]);
		obs_property_list_add_string(list, scene ? obs_source_get_name(scene) : uuids[i], uuids[i]);
		char k[VF_UUID_LEN + 2];
		key_for(k, sizeof(k), 'w', uuids[i]);
		obs_property_int_set_suffix(obs_properties_add_int(sizes, k, obs_module_text("Size.Width"), 0, 8192, 1),
					    " px");
		key_for(k, sizeof(k), 'h', uuids[i]);
		obs_property_int_set_suffix(
			obs_properties_add_int(sizes, k, obs_module_text("Size.Height"), 0, 8192, 1), " px");
		obs_source_release(scene);
	}
	obs_property_set_modified_callback(list, edit_scene_changed);
	if (f && n)
		select_edit_scene(f, uuids, n);
	obs_properties_add_group(props, "sizes_group", obs_module_text("Size.Scenes"), OBS_GROUP_NORMAL, sizes);
	vf_add_about(props, "BrowserReSize");
	return props;
}

static void vs_filter_add(void *data, obs_source_t *parent)
{
	struct vf_size *f = data;
	pthread_mutex_lock(&f->lock);
	const bool on = f->redraw;
	f->redraw_sent = on;
	pthread_mutex_unlock(&f->lock);
	queue_redraw(parent, on);
}

/* Also called while the browser source itself is destroyed (closing OBS, another collection): its CSS stays then. */
static void vs_filter_remove(void *data, obs_source_t *parent)
{
	struct vf_size *f = data;
	obs_source_t *ref = obs_source_get_ref(parent); /* NULL once the source is being destroyed */
	if (!ref)
		return;
	pthread_mutex_lock(&f->lock);
	const bool sent = f->redraw_sent;
	f->redraw_sent = false;
	pthread_mutex_unlock(&f->lock);
	if (sent && !obs_source_removed(ref))
		queue_redraw(ref, false);
	obs_source_release(ref);
}

static void vs_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_SMOOTH, true);
	obs_data_set_default_bool(settings, S_FIXED_HEIGHT, false);
	obs_data_set_default_bool(settings, S_REDRAW, true);
	obs_data_set_default_int(settings, S_ANCHOR, 7);
}

struct obs_source_info vf_size_filter_info = {
	.id = "vakovsky_browser_resize",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = vs_get_name,
	.create = vs_create,
	.destroy = vs_destroy,
	.update = vs_update,
	.video_tick = vs_tick,
	.video_render = vs_render,
	.get_width = vs_get_width,
	.get_height = vs_get_height,
	.get_properties = vs_properties,
	.get_defaults = vs_defaults,
	.filter_add = vs_filter_add,
	.filter_remove = vs_filter_remove,
};
