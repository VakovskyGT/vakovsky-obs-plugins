#include "profile-prune.h"

#include <obs-frontend-api.h>
#include <util/darray.h>

#include <string.h>

struct prune_ctx {
	const char *id;
	DARRAY(obs_source_t *) filters;
};

static void collect_filter(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);
	struct prune_ctx *c = param;
	if (strcmp(obs_source_get_unversioned_id(filter), c->id) == 0) {
		obs_source_t *ref = obs_source_get_ref(filter);
		da_push_back(c->filters, &ref);
	}
}

static bool collect_source(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, collect_filter, param);
	return true;
}

void vf_for_each_filter(const char *filter_id, void (*fn)(obs_source_t *filter))
{
	/* collect first, as below: fn must not run while libobs holds its source list */
	struct prune_ctx c = {filter_id};
	da_init(c.filters);
	obs_enum_sources(collect_source, &c);
	for (size_t i = 0; i < c.filters.num; i++) {
		fn(c.filters.array[i]);
		obs_source_release(c.filters.array[i]);
	}
	da_free(c.filters);
}

void vf_prune_profiles_of(const char *filter_id,
			  void (*prune)(obs_source_t *filter, const char *const *known, size_t n))
{
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	DARRAY(const char *) known;
	da_init(known);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const char *uuid = obs_source_get_uuid(scenes.sources.array[i]);
		da_push_back(known, &uuid);
	}

	/* collect first: a filter's cleanup must not run while libobs holds its source list */
	struct prune_ctx c = {filter_id};
	da_init(c.filters);
	obs_enum_sources(collect_source, &c);
	for (size_t i = 0; i < c.filters.num; i++) {
		obs_source_t *filter = c.filters.array[i];
		prune(filter, known.array, known.num);
		obs_source_release(filter);
	}
	da_free(c.filters);
	da_free(known);
	obs_frontend_source_list_free(&scenes);
}
