/* vf_params <-> obs_data settings (flat keys), and per-scene profile storage. */
#pragma once

#include <obs-data.h>

#include "vf-params.h"

/* Flat settings keys: the editor fields of the currently edited profile. Colors are stored sRGB, params hold linear. */
void vf_params_from_data(obs_data_t *d, struct vf_params *p);
void vf_params_to_data(const struct vf_params *p, obs_data_t *d);
void vf_params_set_defaults(obs_data_t *d);

/* The old "anim_mode" setting, read only to carry it over into anim_on. */
enum vf_anim_mode { VF_ANIM_TRANSITION = 0, VF_ANIM_CUSTOM = 1, VF_ANIM_OFF = 2 };

struct vf_profile {
	char uuid[40];
	struct vf_params params;
};

struct vf_store {
	bool per_scene;
	struct vf_params def;
	struct vf_profile *scenes;
	size_t count;
	bool anim_on; /* "smooth change": animate along with the OBS transition */
	int anim_ms;  /* duration for Cut, which has none of its own */
};

/* Settings keys: per_scene, edit_scene ("" = default profile), default (obj), scenes (obj uuid -> obj),
 * anim_on (old anim_mode carried over), anim_ms. Profiles store only what was saved; missing keys come from defaults. */
void vf_store_load(obs_data_t *settings, struct vf_store *out);
/* UI thread: writes anim_on from an old anim_mode and the backing block from an old "out_back" into the flat fields
 * and every profile, so the editor shows what vf_store_load does. */
void vf_store_migrate(obs_data_t *settings);
/* UI thread, for a scene used as a source (OBS clips everything past its edge): turns the outer border and the outer
 * shadow off in the flat fields and every profile; an inner shadow keeps its state. Returns whether anything shown
 * changed. */
bool vf_store_drop_outer(obs_data_t *settings);
void vf_store_free(struct vf_store *s);
const struct vf_params *vf_store_find(const struct vf_store *s, const char *uuid);
void vf_store_set_defaults(obs_data_t *settings);

/* Flat editor fields <-> the edited profile (default when per_scene is off or edit_scene is ""). */
void vf_store_save_edit(obs_data_t *settings);
/* Saves the flat fields into the profile `key` ("" = default), whatever edit_scene says now. */
void vf_store_save_edit_to(obs_data_t *settings, const char *key);
/* The same, only when the fields differ from what profile `key` gives now: leaving a scene that was not edited must not
 * turn its inherited values into a profile of its own. Returns true when it saved. */
bool vf_store_save_edit_if_changed(obs_data_t *settings, const char *key);
void vf_store_load_edit(obs_data_t *settings);
/* Returns true when edit_scene named a dropped scene and was reset to "" (the fields must be reloaded). */
bool vf_store_drop_unknown(obs_data_t *settings, const char *const *known_uuids, size_t n);
/* "Apply to all scenes": one block of the edited profile (the flat fields) into the default profile and every scene
 * profile; their other blocks stay. */
void vf_store_apply_group(obs_data_t *settings, enum vf_group g);
/* Editor button: removes the edited scene profile, so the scene follows the default again. */
void vf_store_reset_edit(obs_data_t *settings);
