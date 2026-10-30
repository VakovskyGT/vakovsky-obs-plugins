/*
Ultimate Frame
Copyright (C) 2026 VakovskyGT

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "copy-registry.h"
#include "profile-prune.h"
#include "scene-anim.h"
#include "update-check.h"
#include "scene-scope.h"

/* Browser ReSize module: the Browser ReSize filter and its scene scope. */
extern struct obs_source_info vf_size_filter_info;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* the name and description the plugin manager of OBS 33 shows */
const char *obs_module_name(void)
{
	return obs_module_text("BrowserReSize");
}

const char *obs_module_description(void)
{
	return obs_module_text("Module.Description");
}

bool obs_module_load(void)
{
	obs_register_source(&vf_size_filter_info);
	vf_scene_scope_register("vakovsky_browser_resize_scope", "BrowserReSize");
	obs_log(LOG_INFO, "loaded %s", PLUGIN_VERSION);
	return true;
}

static void tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(seconds);
	vf_registry_tick();
}

static void switch_started(obs_source_t *from, const char *from_uuid, obs_source_t *dest, const char *dest_uuid)
{
	vf_scene_scope_attach(from, from_uuid);
	vf_scene_scope_attach(dest, dest_uuid);
}

static void collection_loaded(void)
{
	vf_prune_profiles_of("vakovsky_browser_resize", vf_size_prune);
}

void vf_size_follow_scene(obs_source_t *filter);

static void follow_scene(void)
{
	vf_for_each_filter("vakovsky_browser_resize", vf_size_follow_scene);
}

void obs_module_post_load(void)
{
	static const struct vf_anim_hooks hooks = {
		.switch_started = switch_started,
		.switch_stopped = vf_scene_scope_clear,
		.collection_loaded = collection_loaded,
		.scene_changed = follow_scene,
	};
	vf_registry_init();
	vf_anim_init(&hooks);
	obs_add_tick_callback(tick, NULL);
	vf_update_start("browser-resize-v");
}

void obs_module_unload(void)
{
	vf_update_stop();
	obs_remove_tick_callback(tick, NULL);
	vf_anim_free();
	vf_registry_free();
	obs_log(LOG_INFO, "plugin unloaded");
}
