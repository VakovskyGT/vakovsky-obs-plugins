/* The author line at the bottom of every filter's and transition's properties. */
#pragma once

#include <obs-module.h>

#define VF_URL_REPO "https://github.com/VakovskyGT/vakovsky-obs-plugins"
#define VF_URL_TWITCH "https://www.twitch.tv/vakovsky"
#define VF_URL_DONATE "https://www.donationalerts.com/r/vakovsky"
#define VF_URL_BOOSTY "https://boosty.to/vakovsky"

/* name_key: the locale key of the source's display name. */
void vf_add_about(obs_properties_t *props, const char *name_key);
