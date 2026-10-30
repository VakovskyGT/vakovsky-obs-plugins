/* Effect parameters (all sizes in canvas pixels), defaults, interpolation and easing. Pure: no OBS runtime. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum vf_fill { VF_FILL_SOLID = 0, VF_FILL_LINEAR = 1 };
enum vf_shadow_type { VF_SHADOW_OUTER = 0, VF_SHADOW_INNER = 1 };
enum vf_easing { VF_EASE_LINEAR = 0, VF_EASE_SMOOTH = 1 };
enum vf_corner { VF_TL = 0, VF_TR = 1, VF_BR = 2, VF_BL = 3 };
enum vf_crop_side { VF_CROP_L = 0, VF_CROP_T = 1, VF_CROP_R = 2, VF_CROP_B = 3 };

/* Linear RGB, straight alpha, 0..1 */
struct vf_color {
	float r, g, b, a;
};

struct vf_stroke {
	float width, softness, opacity; /* canvas px, canvas px, 0..1 */
	enum vf_fill fill;
	struct vf_color c0, c1; /* c1 and angle_deg only for VF_FILL_LINEAR */
	float angle_deg;
};

struct vf_params {
	/* each block can be switched off; vf_params_effective turns an off block into its zero state */
	bool corners_on, inner_on, outer_on, crop_on, content_on;
	bool per_corner;
	float radius;    /* shared radius, canvas px */
	float corner[4]; /* VF_TL..VF_BL, used when per_corner */
	struct vf_stroke inner, outer;
	bool shadow_on;
	enum vf_shadow_type shadow_type;
	struct vf_color shadow_color;
	float shadow_opacity, shadow_blur, shadow_x, shadow_y;
	float zoom;                 /* 1.0 = 100 % */
	float content_x, content_y; /* canvas px */
	float crop[4];              /* VF_CROP_L..VF_CROP_B, source px */
	bool backing_on;            /* fill under the source, within the shape */
	struct vf_stroke backing;   /* its fill and opacity; width and softness unused */
	bool edge_on, per_side;     /* fade to transparency at the edges */
	float edge;                 /* shared fade width, canvas px */
	float edge_side[4];         /* VF_CROP_L..VF_CROP_B, used when per_side */
};

/* The blocks of the editor, as "apply to all scenes" copies them. */
enum vf_group {
	VF_GROUP_CROP,
	VF_GROUP_CORNERS,
	VF_GROUP_INNER,
	VF_GROUP_OUTER,
	VF_GROUP_SHADOW,
	VF_GROUP_CONTENT,
	VF_GROUP_EDGE,
	VF_GROUP_BACKING
};

void vf_params_default(struct vf_params *p);
/* Copies one block's values from src into dst, leaving the rest of dst as it is. */
void vf_params_copy_group(struct vf_params *dst, const struct vf_params *src, enum vf_group g);
void vf_params_lerp(const struct vf_params *a, const struct vf_params *b, float t, struct vf_params *out);
float vf_ease(enum vf_easing e, float t);
/* Move's curve: easing 0 none, 1 in, 2 out, 3 in-out; fn 1..10 as Move's easing_function (quadratic..back). */
float vf_ease_move(int easing, int fn, float t);
/* Eased progress 0..1 of a scene-switch animation, or -1 when none runs. transition_ms 0 = a fixed transition (Cut):
 * cut_ms is used instead. move_easing < 0 or move_fn <= 0: the smooth curve. */
float vf_anim_progress(bool on, uint32_t transition_ms, int cut_ms, int move_easing, int move_fn, double elapsed_ms);
void vf_params_radii(const struct vf_params *p, float out[4]);
float vf_params_outer_extent(const struct vf_params *p);
/* Fade widths per side (VF_CROP_L..VF_CROP_B), canvas px. */
void vf_params_edge_widths(const struct vf_params *p, float out[4]);
/* How far the outer effects reach past each side (VF_CROP_L..VF_CROP_B), canvas px: the edge fade starts there. */
void vf_params_outer_extents(const struct vf_params *p, float out[4]);
bool vf_params_has_outer(const struct vf_params *p);
bool vf_params_needs_full(const struct vf_params *p);
bool vf_params_is_noop(const struct vf_params *p);
/* Zero state of the blocks switched off (corners, strokes, backing, crop, zoom and offset); the flags stay. */
void vf_params_effective(struct vf_params *p);
/* Drops effects drawn beyond the source edge (outer stroke, outer shadow). */
void vf_params_strip_outer(struct vf_params *p);
/* Resolution factor (0.05..1) to capture the source at, from its on-canvas scale and the content zoom. */
float vf_capture_scale(float sx, float sy, float zoom);
float vf_pad_source_px(float extent_canvas, float sx, float sy);
/* The cropped part of a w x h source: offset (ox, oy) and size (cw, ch), at least 2 x 2 px; the whole source when it
 * is smaller than that. */
void vf_params_crop_rect(const struct vf_params *p, float w, float h, float *ox, float *oy, float *cw, float *ch);
/* The frame shape in item px: the cropped part (cw x ch) minus OBS's own item crop (alt: L, T, R, B). clip_w/clip_h:
 * the size OBS clips the item at when it draws it through a texture (the reported size), 0 = no clip. */
void vf_shape_rect(float cw, float ch, const int alt[4], float clip_w, float clip_h, float out[4]);
