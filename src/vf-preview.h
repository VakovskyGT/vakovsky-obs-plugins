/* The filters dialog, the properties dialog and a source projector draw the source alone, filling their window: there
 * the filter shows the source as its scene shows it, outer effects included, fitted into the window. Pure. */
#pragma once

#include <stdbool.h>

#include "vf-match.h"

struct vf_preview_in {
	enum vf_match_kind match; /* how a scene copy was identified */
	bool editor_open;         /* the filter's properties are shown (the filters dialog has it selected) */
	int view_w, view_h;       /* the viewport */
	unsigned src_w, src_h;    /* the size the source reports */
	unsigned canvas_w, canvas_h;
	float m[16]; /* the model matrix (libobs matrix4 as 16 floats: x, y, z, t rows) */
};

/* The dialogs draw the source alone with an identity matrix, scaled to their window with the source's aspect (on macOS
 * through a texture, so a render target is no sign of it). An identity matrix also passes as any uniformly scaled
 * copy, so only an exact match may be a real scene item: an item at (0, 0) at 100%. That one counts as the dialog only
 * while the editor is open, the viewport has the source's aspect and is not the canvas (the output, a transition).
 * Studio Mode preview of such an item with the canvas's aspect is still taken for the dialog while it is open. A
 * render at the source's own size (a source clone) is left alone. */
bool vf_preview_is_source_view(const struct vf_preview_in *in);
/* Scale k (<= 1) and offset (tx, ty) that fit a w x h source plus ext_x / ext_y source px of outer effects on every
 * side into the w x h view, centred. */
void vf_preview_fit(float w, float h, float ext_x, float ext_y, float *k, float *tx, float *ty);
