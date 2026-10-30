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

#include <util/platform.h>

/* Ultimate Frame module: the Ultimate Frame filter and Fade for Ultimate Frame. */
extern struct obs_source_info vf_frame_filter_info;
extern struct obs_source_info vf_fade_transition_info;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* the name and description the plugin manager of OBS 33 shows */
const char *obs_module_name(void)
{
	return obs_module_text("UltimateFrame");
}

const char *obs_module_description(void)
{
	return obs_module_text("Module.Description");
}

bool obs_module_load(void)
{
	obs_register_source(&vf_frame_filter_info);
	obs_register_source(&vf_fade_transition_info);
	vf_scene_scope_register("vakovsky_ultimate_frame_scope", "UltimateFrame");
	obs_log(LOG_INFO, "loaded %s", PLUGIN_VERSION);
	return true;
}

void vf_filters_release_idle(uint64_t now_ns);

static void tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(seconds);
	static uint64_t last_idle_check;
	vf_registry_tick();

	uint64_t now = os_gettime_ns();
	if (now - last_idle_check > 1000000000ULL) {
		last_idle_check = now;
		vf_filters_release_idle(now);
	}
}

static void prune(void)
{
	vf_prune_profiles_of("vakovsky_ultimate_frame", vf_frame_prune);
}

void vf_frame_follow_scene(obs_source_t *filter);

/* Each side of a transition is told apart even where the source sits in the same place in both scenes (Fade without
 * "smooth change" shows each scene's own look). */
static void switch_started(obs_source_t *from, const char *from_uuid, obs_source_t *dest, const char *dest_uuid)
{
	vf_scene_scope_attach(from, from_uuid);
	vf_scene_scope_attach(dest, dest_uuid);
}

static void follow_scene(void)
{
	vf_for_each_filter("vakovsky_ultimate_frame", vf_frame_follow_scene);
}

void obs_module_post_load(void)
{
	static const struct vf_anim_hooks hooks = {
		.switch_started = switch_started,
		.switch_stopped = vf_scene_scope_clear,
		.collection_loaded = prune,
		.scene_changed = follow_scene,
	};
	vf_registry_init();
	vf_anim_init(&hooks);
	obs_add_tick_callback(tick, NULL);
	vf_update_start("ultimate-frame-v");
}

void obs_module_unload(void)
{
	vf_update_stop();
	obs_remove_tick_callback(tick, NULL);
	vf_anim_free();
	vf_registry_free();
	obs_log(LOG_INFO, "plugin unloaded");
}
