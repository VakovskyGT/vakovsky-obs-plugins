#include "scene-anim.h"

#include <obs-frontend-api.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#include <plugin-support.h>

#include "profile-prune.h"

static pthread_mutex_t anim_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct vf_anim_state anim;

static void copy_uuid(char *dst, obs_source_t *scene)
{
	snprintf(dst, VF_UUID_LEN, "%s", scene ? obs_source_get_uuid(scene) : "");
}

static uint32_t current_transition_ms(void)
{
	obs_source_t *tr = obs_frontend_get_current_transition();
	bool fixed = tr ? obs_transition_fixed(tr) : true;
	obs_source_release(tr);
	return fixed ? 0 : (uint32_t)obs_frontend_get_transition_duration();
}

/* A scene's "Transition Override" (frontend private settings) replaces the transition and its duration. */
#define VF_OVERRIDE_DEFAULT_MS 300

static uint32_t transition_ms_to(obs_source_t *tr, obs_source_t *scene)
{
	if (!tr || obs_transition_fixed(tr))
		return 0;
	uint32_t ms = (uint32_t)obs_frontend_get_transition_duration();
	obs_data_t *priv = scene ? obs_source_get_private_settings(scene) : NULL;
	const char *name = priv ? obs_data_get_string(priv, "transition") : NULL;
	if (name && *name && strcmp(name, obs_source_get_name(tr)) == 0)
		ms = obs_data_has_user_value(priv, "transition_duration")
			     ? (uint32_t)obs_data_get_int(priv, "transition_duration")
			     : VF_OVERRIDE_DEFAULT_MS;
	obs_data_release(priv);
	return ms;
}

/* program: the scene that is (or is becoming) the program scene; NULL = ask the frontend.
 * ms: duration of the switching transition, or UINT32_MAX = the current transition's. */
static void refresh_scenes_to(obs_source_t *program_hint, bool program_switched, uint32_t ms, int easing, int easing_fn)
{
	obs_source_t *program = program_hint ? obs_source_get_ref(program_hint) : obs_frontend_get_current_scene();
	obs_source_t *preview = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									   : NULL;
	if (!program_switched)
		ms = 0;
	else if (ms == UINT32_MAX)
		ms = current_transition_ms();

	pthread_mutex_lock(&anim_mutex);
	if (program_switched) {
		char next[VF_UUID_LEN];
		copy_uuid(next, program);
		if (strcmp(next, anim.program_uuid) != 0) {
			memcpy(anim.from_uuid, anim.program_uuid, VF_UUID_LEN);
			memcpy(anim.program_uuid, next, VF_UUID_LEN);
			anim.start_ns = anim.from_uuid[0] ? os_gettime_ns() : 0;
			anim.transition_ms = ms;
			anim.move_easing = easing;
			anim.move_easing_fn = easing_fn;
		} else if (program_hint && anim.start_ns) {
			/* The transition signal supplies the actual override, even if the frontend announced the scene first. */
			anim.transition_ms = ms;
			anim.move_easing = easing;
			anim.move_easing_fn = easing_fn;
		}
	} else {
		copy_uuid(anim.program_uuid, program);
	}
	copy_uuid(anim.preview_uuid, preview);
	pthread_mutex_unlock(&anim_mutex);

	obs_source_release(preview);
	obs_source_release(program);
}

static void move_easing(obs_source_t *tr, int *easing, int *fn);

static void refresh_scenes(bool program_switched)
{
	obs_source_t *tr = obs_frontend_get_current_transition();
	int easing, fn;
	move_easing(tr, &easing, &fn);
	refresh_scenes_to(NULL, program_switched, UINT32_MAX, easing, fn);
	obs_source_release(tr);
}

/* Move moves matched items along its "Easing" curve; sizes that change with it must follow the same curve. */
static void move_easing(obs_source_t *tr, int *easing, int *fn)
{
	*easing = -1;
	*fn = 0;
	if (!tr || strcmp(obs_source_get_unversioned_id(tr), "move_transition") != 0)
		return;
	obs_data_t *s = obs_source_get_settings(tr);
	*easing = (int)obs_data_get_int(s, "easing_match");
	*fn = (int)obs_data_get_int(s, "easing_function_match");
	obs_data_release(s);
}

static struct vf_anim_hooks hooks;

static void switch_stopped(void)
{
	if (hooks.switch_stopped)
		hooks.switch_stopped();
}

/* OBS reports SCENE_CHANGED when a timed transition ends; the animation must start with the transition. */
static void on_transition_start(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	obs_source_t *tr = calldata_ptr(cd, "source");
	obs_source_t *dest = tr ? obs_transition_get_source(tr, OBS_TRANSITION_SOURCE_B) : NULL;
	if (dest && obs_source_is_scene(dest)) {
		/* Studio Mode with "Duplicate Scene" transitions to a private copy of the scene; the frontend has
		 * already made the original the program scene by then */
		obs_source_t *scene = obs_obj_is_private(dest) ? obs_frontend_get_current_scene()
							       : obs_source_get_ref(dest);
		int easing, fn;
		move_easing(tr, &easing, &fn);
		refresh_scenes_to(scene, true, transition_ms_to(tr, scene), easing, fn);
		struct vf_anim_state current;
		vf_anim_get(&current);
		if (hooks.switch_started) {
			obs_source_t *from = obs_transition_get_source(tr, OBS_TRANSITION_SOURCE_A);
			hooks.switch_started(from, current.from_uuid, dest, current.program_uuid);
			obs_source_release(from);
		}
		obs_source_release(scene);
	}
	obs_source_release(dest);
}

/* Frontend transitions are private sources, which never emit the core "source_transition_start" signal: listen on
 * every frontend transition itself (a scene's Transition Override uses one that is not the current transition). */
static DARRAY(obs_weak_source_t *) hooked;

static void unhook_transitions(void)
{
	for (size_t i = 0; i < hooked.num; i++) {
		obs_source_t *old = obs_weak_source_get_source(hooked.array[i]);
		if (old)
			signal_handler_disconnect(obs_source_get_signal_handler(old), "transition_start",
						  on_transition_start, NULL);
		obs_source_release(old);
		obs_weak_source_release(hooked.array[i]);
	}
	da_free(hooked);
}

static void hook_transitions(void)
{
	unhook_transitions();
	struct obs_frontend_source_list list = {0};
	obs_frontend_get_transitions(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *tr = list.sources.array[i];
		signal_handler_connect(obs_source_get_signal_handler(tr), "transition_start", on_transition_start,
				       NULL);
		obs_weak_source_t *weak = obs_source_get_weak_source(tr);
		da_push_back(hooked, &weak);
	}
	obs_frontend_source_list_free(&list);
}

/* OBS drops every frontend callback before modules unload; removing ours later only logs a warning. */
static bool frontend_hooked;

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		refresh_scenes(true);
		if (hooks.scene_changed)
			hooks.scene_changed();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		switch_stopped();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
		hook_transitions();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		switch_stopped();
		unhook_transitions();
		/* safe here: the frontend walks its callbacks from the end */
		obs_frontend_remove_event_callback(on_frontend_event, NULL);
		frontend_hooked = false;
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		switch_stopped();
		unhook_transitions();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		switch_stopped();
		if (hooks.collection_loaded)
			hooks.collection_loaded();
		hook_transitions();
		pthread_mutex_lock(&anim_mutex);
		memset(&anim, 0, sizeof(anim));
		pthread_mutex_unlock(&anim_mutex);
		refresh_scenes(false);
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		refresh_scenes(false);
		if (hooks.scene_changed)
			hooks.scene_changed();
		break;
	default:
		break;
	}
}

void vf_anim_init(const struct vf_anim_hooks *h)
{
	if (h)
		hooks = *h;
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	frontend_hooked = true;
}

void vf_anim_free(void)
{
	switch_stopped();
	unhook_transitions();
	if (frontend_hooked)
		obs_frontend_remove_event_callback(on_frontend_event, NULL);
	frontend_hooked = false;
}

void vf_anim_get(struct vf_anim_state *out)
{
	pthread_mutex_lock(&anim_mutex);
	*out = anim;
	pthread_mutex_unlock(&anim_mutex);
}
