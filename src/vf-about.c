#include "vf-about.h"

#include <plugin-support.h>
#include <util/dstr.h>

#include "update-check.h"

static void link(struct dstr *s, const char *url, const char *text)
{
	if (url[0])
		dstr_catf(s, "<a href=\"%s\">%s</a>", url, text);
	else
		dstr_cat(s, text);
}

static bool update_skip_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);
	UNUSED_PARAMETER(data);
	vf_update_skip();
	/* OBS refreshes the open dialog from these props without asking for new ones: hide the release here */
	obs_property_set_visible(obs_properties_get(props, "vaki_update"), false);
	obs_property_set_visible(obs_properties_get(props, "vaki_update_open"), false);
	obs_property_set_visible(obs_properties_get(props, "vaki_update_skip"), false);
	return true;
}

/* A newer release (found by update-check.c): a line, a link to its page and a button to skip it. Skipping lives in
 * the module's config, not in the filter's settings: it is shared by every filter. */
static void add_update(obs_properties_t *props)
{
	char version[32], url[512];
	if (!vf_update_available(version, sizeof(version), url, sizeof(url)))
		return;
	struct dstr s = {0};
	dstr_printf(&s, obs_module_text("Update.Available"), version);
	obs_property_t *t = obs_properties_add_text(props, "vaki_update", s.array, OBS_TEXT_INFO);
	obs_property_text_set_info_type(t, OBS_TEXT_INFO_WARNING);
	dstr_free(&s);
	obs_property_t *b =
		obs_properties_add_button2(props, "vaki_update_open", obs_module_text("Update.Open"), NULL, NULL);
	obs_property_button_set_type(b, OBS_BUTTON_URL);
	obs_property_button_set_url(b, url);
	obs_properties_add_button2(props, "vaki_update_skip", obs_module_text("Update.Skip"), update_skip_clicked,
				   NULL);
}

void vf_add_about(obs_properties_t *props, const char *name_key)
{
	add_update(props);

	struct dstr s = {0};
	link(&s, VF_URL_REPO, obs_module_text(name_key));
	dstr_catf(&s, " %s %s ", PLUGIN_VERSION, obs_module_text("About.By"));
	link(&s, VF_URL_TWITCH, "Vakovsky");

	dstr_catf(&s, "<br>%s ", obs_module_text("About.Support"));
	link(&s, VF_URL_DONATE, obs_module_text("About.Donate"));
	dstr_cat(&s, " · ");
	link(&s, VF_URL_BOOSTY, "Boosty");

	obs_properties_add_text(props, "vaki_about", s.array, OBS_TEXT_INFO);
	dstr_free(&s);
}
