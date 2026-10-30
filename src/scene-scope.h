/* A temporary pass-through filter on each scene of a running transition tells the module's filter which scene copy is
 * being drawn (vf_fade_scene_get); cleared when the transition stops. Each module has its own, with its own id. */
#pragma once

#include <obs.h>

/* obs_module_load: registers the scope filter under this id; name_key is its locale key */
void vf_scene_scope_register(const char *id, const char *name_key);

void vf_scene_scope_attach(obs_source_t *scene, const char *uuid);
void vf_scene_scope_clear(void);
