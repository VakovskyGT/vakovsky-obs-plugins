/* The redraw block Browser ReSize keeps in a browser source's custom CSS: an invisible animation that makes the page
 * draw every frame, so a static page shows its new size (Chromium with GPU acceleration draws only on changes). Pure. */
#pragma once

#include <stdbool.h>

#define VF_CSS_BEGIN "/* Browser ReSize: redraw (managed by the filter, do not edit) */"
#define VF_CSS_END "/* Browser ReSize: end */"

bool vf_css_has_redraw(const char *css);
/* The CSS with the block added (on) or cut out (off), malloc'd; NULL when nothing changes. The rest stays as it is. */
char *vf_css_set_redraw(const char *css, bool on);
