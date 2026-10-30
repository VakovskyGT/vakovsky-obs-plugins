/* The scene scope filter: tells the module's filter which scene copy a transition is drawing. */
#include <obs-module.h>
#include <util/darray.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#include "copy-registry.h"
#include "scene-scope.h"
#include "vf-fade.h"

/* Scene scopes work with the native transition renderer, without replacing its shader or audio. They report no size of
 * their own: libobs then takes the next filter's, so a crop on the scene keeps its size. */
static const char *scope_id; /* set once in obs_module_load */
static const char *scope_name_key;
struct scene_scope {
	obs_source_t *self;
	char uuid[VF_UUID_LEN];
};
static void scope_update(void *data, obs_data_t *settings)
{
	struct scene_scope *s = data;
	snprintf(s->uuid, sizeof(s->uuid), "%s", obs_data_get_string(settings, "scene_uuid"));
}
static void *scope_create(obs_data_t *settings, obs_source_t *self)
{
	struct scene_scope *s = bzalloc(sizeof(*s));
	s->self = self;
	scope_update(s, settings);
	return s;
}
static void scope_destroy(void *data)
{
	bfree(data);
}
static void scope_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct scene_scope *s = data;
	const char *previous = vf_fade_scene_push(s->uuid);
	obs_source_skip_video_filter(s->self);
	vf_fade_scene_push(previous);
}
static void find_scope(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);
	obs_source_t **found = param;
	if (!*found && strcmp(obs_source_get_unversioned_id(filter), scope_id) == 0)
		*found = obs_source_get_ref(filter);
}
static pthread_mutex_t scope_lock = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(obs_weak_source_t *) scoped;
void vf_scene_scope_attach(obs_source_t *scene, const char *uuid)
{
	if (!scene || !obs_source_is_scene(scene) || !uuid || !*uuid)
		return;
	pthread_mutex_lock(&scope_lock);
	obs_source_t *filter = NULL;
	obs_source_enum_filters(scene, find_scope, &filter);
	if (!filter) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "scene_uuid", uuid);
		filter = obs_source_create_private(scope_id, "scene scope", d);
		obs_data_release(d);
		if (filter)
			obs_source_filter_add(scene, filter);
	}
	if (filter) {
		obs_weak_source_t *weak = obs_source_get_weak_source(scene);
		bool seen = false;
		for (size_t i = 0; i < scoped.num; i++)
			if (scoped.array[i] == weak)
				seen = true;
		if (seen)
			obs_weak_source_release(weak);
		else
			da_push_back(scoped, &weak);
	}
	obs_source_release(filter);
	pthread_mutex_unlock(&scope_lock);
}
static bool clear_saved_scope(void *unused, obs_source_t *scene)
{
	UNUSED_PARAMETER(unused);
	obs_source_t *filter = NULL;
	obs_source_enum_filters(scene, find_scope, &filter);
	if (filter)
		obs_source_filter_remove(scene, filter);
	obs_source_release(filter);
	return true;
}

void vf_scene_scope_clear(void)
{
	pthread_mutex_lock(&scope_lock);
	for (size_t i = 0; i < scoped.num; i++) {
		obs_source_t *scene = obs_weak_source_get_source(scoped.array[i]);
		obs_source_t *filter = NULL;
		if (scene)
			obs_source_enum_filters(scene, find_scope, &filter);
		if (filter)
			obs_source_filter_remove(scene, filter);
		obs_source_release(filter);
		obs_source_release(scene);
		obs_weak_source_release(scoped.array[i]);
	}
	da_free(scoped);
	/* Older test builds could save a temporary scope with its scene. */
	obs_enum_scenes(clear_saved_scope, NULL);
	pthread_mutex_unlock(&scope_lock);
}
static const char *scope_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text(scope_name_key);
}

void vf_scene_scope_register(const char *id, const char *name_key)
{
	scope_id = id;
	scope_name_key = name_key;
	struct obs_source_info info = {
		.id = id,
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_CAP_OBSOLETE,
		.get_name = scope_name,
		.create = scope_create,
		.destroy = scope_destroy,
		.update = scope_update,
		.video_render = scope_render,
	};
	obs_register_source(&info);
}
