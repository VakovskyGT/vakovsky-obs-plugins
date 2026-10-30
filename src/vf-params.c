#include "vf-params.h"

#include <math.h>

#define VF_PI 3.14159265358979323846

static const struct vf_color VF_WHITE = {1.0f, 1.0f, 1.0f, 1.0f};
static const struct vf_color VF_BLACK = {0.0f, 0.0f, 0.0f, 1.0f};

static void stroke_default(struct vf_stroke *s)
{
	s->width = 0.0f;
	s->softness = 1.0f;
	s->opacity = 1.0f;
	s->fill = VF_FILL_SOLID;
	s->c0 = VF_WHITE;
	s->c1 = VF_BLACK;
	s->angle_deg = 90.0f;
}

void vf_params_default(struct vf_params *p)
{
	p->corners_on = p->inner_on = p->outer_on = p->crop_on = p->content_on = true;
	p->per_corner = false;
	p->radius = 50.0f;
	for (int i = 0; i < 4; i++)
		p->corner[i] = 50.0f;
	stroke_default(&p->inner);
	stroke_default(&p->outer);
	p->shadow_on = false;
	p->shadow_type = VF_SHADOW_OUTER;
	p->shadow_color = VF_BLACK;
	p->shadow_opacity = 0.6f;
	p->shadow_blur = 30.0f;
	p->shadow_x = 15.0f;
	p->shadow_y = 15.0f;
	p->zoom = 1.0f;
	p->content_x = 0.0f;
	p->content_y = 0.0f;
	for (int i = 0; i < 4; i++)
		p->crop[i] = 0.0f;
	p->backing_on = false;
	stroke_default(&p->backing);
	p->backing.softness = 0.0f;
	p->backing.c0 = VF_BLACK;
	p->backing.c1 = VF_WHITE;
	p->edge_on = false;
	p->per_side = false;
	p->edge = 50.0f;
	for (int i = 0; i < 4; i++)
		p->edge_side[i] = 50.0f;
}

void vf_params_copy_group(struct vf_params *dst, const struct vf_params *src, enum vf_group g)
{
	switch (g) {
	case VF_GROUP_CROP:
		dst->crop_on = src->crop_on;
		for (int i = 0; i < 4; i++)
			dst->crop[i] = src->crop[i];
		break;
	case VF_GROUP_CORNERS:
		dst->corners_on = src->corners_on;
		dst->per_corner = src->per_corner;
		dst->radius = src->radius;
		for (int i = 0; i < 4; i++)
			dst->corner[i] = src->corner[i];
		break;
	case VF_GROUP_INNER:
		dst->inner_on = src->inner_on;
		dst->inner = src->inner;
		break;
	case VF_GROUP_OUTER:
		dst->outer_on = src->outer_on;
		dst->outer = src->outer;
		break;
	case VF_GROUP_BACKING:
		dst->backing_on = src->backing_on;
		dst->backing = src->backing;
		break;
	case VF_GROUP_SHADOW:
		dst->shadow_on = src->shadow_on;
		dst->shadow_type = src->shadow_type;
		dst->shadow_color = src->shadow_color;
		dst->shadow_opacity = src->shadow_opacity;
		dst->shadow_blur = src->shadow_blur;
		dst->shadow_x = src->shadow_x;
		dst->shadow_y = src->shadow_y;
		break;
	case VF_GROUP_CONTENT:
		dst->content_on = src->content_on;
		dst->zoom = src->zoom;
		dst->content_x = src->content_x;
		dst->content_y = src->content_y;
		break;
	case VF_GROUP_EDGE:
		dst->edge_on = src->edge_on;
		dst->per_side = src->per_side;
		dst->edge = src->edge;
		for (int i = 0; i < 4; i++)
			dst->edge_side[i] = src->edge_side[i];
		break;
	}
}

void vf_params_effective(struct vf_params *p)
{
	if (!p->corners_on) {
		p->radius = 0.0f;
		for (int i = 0; i < 4; i++)
			p->corner[i] = 0.0f;
	}
	if (!p->inner_on)
		p->inner.width = 0.0f;
	if (!p->outer_on)
		p->outer.width = 0.0f;
	if (!p->backing_on)
		p->backing.opacity = 0.0f;
	if (!p->crop_on)
		for (int i = 0; i < 4; i++)
			p->crop[i] = 0.0f;
	if (!p->content_on) {
		p->zoom = 1.0f;
		p->content_x = 0.0f;
		p->content_y = 0.0f;
	}
	if (!p->edge_on) {
		p->edge = 0.0f;
		for (int i = 0; i < 4; i++)
			p->edge_side[i] = 0.0f;
	}
}

static float clamp01(float t)
{
	return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

static float lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

/* Interpolate premultiplied so a fading-out color does not tint the result. */
static struct vf_color lerp_color(struct vf_color a, struct vf_color b, float t)
{
	float alpha = lerpf(a.a, b.a, t);
	struct vf_color o = {0.0f, 0.0f, 0.0f, alpha};
	if (alpha > 1e-6f) {
		o.r = lerpf(a.r * a.a, b.r * b.a, t) / alpha;
		o.g = lerpf(a.g * a.a, b.g * b.a, t) / alpha;
		o.b = lerpf(a.b * a.a, b.b * b.a, t) / alpha;
	}
	return o;
}

static float lerp_angle(float a, float b, float t)
{
	float d = fmodf(b - a, 360.0f);
	if (d > 180.0f)
		d -= 360.0f;
	else if (d < -180.0f)
		d += 360.0f;
	return a + d * t;
}

static void lerp_stroke(const struct vf_stroke *a, const struct vf_stroke *b, float t, struct vf_stroke *o)
{
	o->width = lerpf(a->width, b->width, t);
	o->softness = lerpf(a->softness, b->softness, t);
	o->opacity = lerpf(a->opacity, b->opacity, t);
	/* a solid fill is a gradient from its color to itself: gradient <-> solid blends instead of switching */
	const bool la = a->fill == VF_FILL_LINEAR, lb = b->fill == VF_FILL_LINEAR;
	o->fill = la || lb ? VF_FILL_LINEAR : VF_FILL_SOLID;
	o->c0 = lerp_color(a->c0, b->c0, t);
	o->c1 = lerp_color(la ? a->c1 : a->c0, lb ? b->c1 : b->c0, t);
	o->angle_deg = lerp_angle(a->angle_deg, b->angle_deg, t);
}

void vf_params_lerp(const struct vf_params *a, const struct vf_params *b, float t, struct vf_params *out)
{
	float ra[4], rb[4], ea[4], eb[4];
	struct vf_params o;

	vf_params_radii(a, ra);
	vf_params_radii(b, rb);
	vf_params_edge_widths(a, ea);
	vf_params_edge_widths(b, eb);

	/* the corners carry the interpolated radii, so the flag never switches midway */
	o.per_corner = a->per_corner || b->per_corner;
	o.radius = lerpf(a->radius, b->radius, t);
	for (int i = 0; i < 4; i++)
		o.corner[i] = lerpf(ra[i], rb[i], t);

	lerp_stroke(&a->inner, &b->inner, t, &o.inner);
	lerp_stroke(&a->outer, &b->outer, t, &o.outer);

	/* a switched-off shadow counts as zero blur, offset and opacity: switching on/off it grows from / shrinks
	 * under the frame, with the switched-on side's type and color. A type change is an off and an on in turn:
	 * the old shadow shrinks away in the first half, the new one grows in the second. */
	o.shadow_on = a->shadow_on || b->shadow_on;
	if (a->shadow_on && b->shadow_on && a->shadow_type != b->shadow_type) {
		const bool first = t < 0.5f;
		const struct vf_params *s = first ? a : b;
		const float k = first ? 1.0f - 2.0f * t : 2.0f * t - 1.0f;
		o.shadow_type = s->shadow_type;
		o.shadow_color = s->shadow_color;
		o.shadow_opacity = s->shadow_opacity * k;
		o.shadow_blur = s->shadow_blur * k;
		o.shadow_x = s->shadow_x * k;
		o.shadow_y = s->shadow_y * k;
	} else {
		const struct vf_params *sa = a->shadow_on ? a : b, *sb = b->shadow_on ? b : a;
		o.shadow_type = sa->shadow_type;
		o.shadow_color = lerp_color(sa->shadow_color, sb->shadow_color, t);
		o.shadow_opacity =
			lerpf(a->shadow_on ? a->shadow_opacity : 0.0f, b->shadow_on ? b->shadow_opacity : 0.0f, t);
		o.shadow_blur = lerpf(a->shadow_on ? a->shadow_blur : 0.0f, b->shadow_on ? b->shadow_blur : 0.0f, t);
		o.shadow_x = lerpf(a->shadow_on ? a->shadow_x : 0.0f, b->shadow_on ? b->shadow_x : 0.0f, t);
		o.shadow_y = lerpf(a->shadow_on ? a->shadow_y : 0.0f, b->shadow_on ? b->shadow_y : 0.0f, t);
	}

	o.zoom = lerpf(a->zoom, b->zoom, t);
	o.content_x = lerpf(a->content_x, b->content_x, t);
	o.content_y = lerpf(a->content_y, b->content_y, t);
	for (int i = 0; i < 4; i++)
		o.crop[i] = lerpf(a->crop[i], b->crop[i], t);

	/* like the shadow: a switched-off backing takes the other side's fill at zero opacity, so it fades in and out in
	 * its own colors */
	struct vf_stroke ba = a->backing, bb = b->backing;
	if (!a->backing_on) {
		ba = b->backing;
		ba.opacity = 0.0f;
	}
	if (!b->backing_on) {
		bb = a->backing;
		bb.opacity = 0.0f;
	}
	o.backing_on = a->backing_on || b->backing_on;
	lerp_stroke(&ba, &bb, t, &o.backing);

	/* like the corners: the sides carry the interpolated widths; a switched-off fade is width 0 (effective) */
	o.edge_on = a->edge_on || b->edge_on;
	o.per_side = a->per_side || b->per_side;
	o.edge = lerpf(a->edge, b->edge, t);
	for (int i = 0; i < 4; i++)
		o.edge_side[i] = lerpf(ea[i], eb[i], t);

	*out = o;
}

float vf_ease(enum vf_easing e, float t)
{
	t = clamp01(t);
	if (e == VF_EASE_SMOOTH)
		return t * t * (3.0f - 2.0f * t);
	return t;
}

/* The ease-in half of each Move function (AHEasing, the formulas Move uses). */
static double ease_in(int fn, double t)
{
	switch (fn) {
	case 1:
		return t * t;
	case 2:
		return t * t * t;
	case 3:
		return t * t * t * t;
	case 4:
		return t * t * t * t * t;
	case 5:
		return 1.0 - cos(t * VF_PI / 2.0);
	case 6:
		return 1.0 - sqrt(1.0 - t * t);
	case 7:
		return t <= 0.0 ? 0.0 : pow(2.0, 10.0 * (t - 1.0));
	case 8:
		return sin(13.0 * VF_PI / 2.0 * t) * pow(2.0, 10.0 * (t - 1.0));
	case 9: {
		/* bounce in is the mirrored bounce out */
		double u = 1.0 - t, out;
		if (u < 4.0 / 11.0)
			out = 121.0 * u * u / 16.0;
		else if (u < 8.0 / 11.0)
			out = 363.0 / 40.0 * u * u - 99.0 / 10.0 * u + 17.0 / 5.0;
		else if (u < 9.0 / 10.0)
			out = 4356.0 / 361.0 * u * u - 35442.0 / 1805.0 * u + 16061.0 / 1805.0;
		else
			out = 54.0 / 5.0 * u * u - 513.0 / 25.0 * u + 268.0 / 25.0;
		return 1.0 - out;
	}
	case 10:
		return t * t * t - t * sin(t * VF_PI);
	default:
		return t;
	}
}

float vf_ease_move(int easing, int fn, float tf)
{
	double t = tf <= 0.0f ? 0.0 : (tf >= 1.0f ? 1.0 : (double)tf);
	if (t <= 0.0 || t >= 1.0)
		return (float)t;
	switch (easing) {
	case 1:
		return (float)ease_in(fn, t);
	case 2:
		return (float)(1.0 - ease_in(fn, 1.0 - t));
	case 3:
		return (float)(t < 0.5 ? 0.5 * ease_in(fn, 2.0 * t) : 1.0 - 0.5 * ease_in(fn, 2.0 - 2.0 * t));
	default:
		return (float)t;
	}
}

float vf_anim_progress(bool on, uint32_t transition_ms, int cut_ms, int move_easing, int move_fn, double elapsed_ms)
{
	double ms = transition_ms ? (double)transition_ms : (double)cut_ms;
	if (!on || ms <= 0.0)
		return -1.0f;
	double raw = elapsed_ms / ms;
	if (raw >= 1.0)
		return -1.0f;
	float t = move_easing < 0 || move_fn <= 0 ? vf_ease(VF_EASE_SMOOTH, (float)raw)
						  : vf_ease_move(move_easing, move_fn, (float)raw);
	/* Move's elastic and back curves overshoot: sizes and opacities must stay in range */
	return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

void vf_params_radii(const struct vf_params *p, float out[4])
{
	for (int i = 0; i < 4; i++)
		out[i] = p->per_corner ? p->corner[i] : p->radius;
}

float vf_params_outer_extent(const struct vf_params *p)
{
	/* the outer border fades out over softness/2 past its width (softness below 1 counts as 1 in frame.effect) */
	float e = p->outer.width > 0.0f ? p->outer.width + 0.5f * fmaxf(p->outer.softness, 1.0f) : 0.0f;
	if (p->shadow_on && p->shadow_type == VF_SHADOW_OUTER) {
		float s = p->shadow_blur + fmaxf(fabsf(p->shadow_x), fabsf(p->shadow_y));
		e = fmaxf(e, s);
	}
	return e;
}

void vf_params_edge_widths(const struct vf_params *p, float out[4])
{
	for (int i = 0; i < 4; i++)
		out[i] = p->per_side ? p->edge_side[i] : p->edge;
}

void vf_params_outer_extents(const struct vf_params *p, float out[4])
{
	const float e = p->outer.width > 0.0f ? p->outer.width + 0.5f * fmaxf(p->outer.softness, 1.0f) : 0.0f;
	for (int i = 0; i < 4; i++)
		out[i] = e;
	if (p->shadow_on && p->shadow_type == VF_SHADOW_OUTER) {
		/* frame.effect: the outer shadow ends blur/2 past the shifted shape */
		const float h = 0.5f * p->shadow_blur;
		const float s[4] = {h - p->shadow_x, h - p->shadow_y, h + p->shadow_x, h + p->shadow_y};
		for (int i = 0; i < 4; i++)
			out[i] = fmaxf(out[i], s[i]);
	}
}

bool vf_params_has_outer(const struct vf_params *p)
{
	return vf_params_outer_extent(p) > 0.0f;
}

bool vf_params_needs_full(const struct vf_params *p)
{
	return p->inner.width > 0.0f || p->outer.width > 0.0f || p->shadow_on ||
	       (p->backing_on && p->backing.opacity > 0.0f);
}

bool vf_params_is_noop(const struct vf_params *p)
{
	float r[4];
	vf_params_radii(p, r);
	bool corners = r[0] > 0.0f || r[1] > 0.0f || r[2] > 0.0f || r[3] > 0.0f;
	bool content = fabsf(p->zoom - 1.0f) > 1e-4f || p->content_x != 0.0f || p->content_y != 0.0f;
	bool cropped = p->crop[0] > 0.0f || p->crop[1] > 0.0f || p->crop[2] > 0.0f || p->crop[3] > 0.0f;
	float e[4];
	vf_params_edge_widths(p, e);
	bool faded = p->edge_on && (e[0] > 0.0f || e[1] > 0.0f || e[2] > 0.0f || e[3] > 0.0f);
	return !corners && !content && !cropped && !faded && !vf_params_needs_full(p);
}

float vf_pad_source_px(float extent_canvas, float sx, float sy)
{
	float s = fminf(sx, sy);
	if (extent_canvas <= 0.0f || s <= 1e-4f)
		return 0.0f;
	return ceilf(extent_canvas / s) + 2.0f;
}

void vf_params_strip_outer(struct vf_params *p)
{
	p->outer.width = 0.0f;
	if (p->shadow_type == VF_SHADOW_OUTER)
		p->shadow_on = false;
}

float vf_capture_scale(float sx, float sy, float zoom)
{
	float s = fmaxf(sx, sy) * fmaxf(zoom, 1.0f);
	return s > 1.0f ? 1.0f : (s < 0.05f ? 0.05f : s);
}

static void crop_axis(float size, float lo, float hi, float *off, float *len)
{
	const float min_len = 2.0f;
	lo = fmaxf(lo, 0.0f);
	hi = fmaxf(hi, 0.0f);
	if (size <= min_len) {
		*off = 0.0f;
		*len = size > 0.0f ? size : 0.0f;
		return;
	}
	float l = size - lo - hi;
	if (l < min_len) {
		/* eats the whole source: keep min_len px, placed where the two crops meet */
		float o = lo / fmaxf(lo + hi, 1e-6f) * (size - min_len);
		*off = o;
		*len = min_len;
		return;
	}
	*off = lo;
	*len = l;
}

void vf_params_crop_rect(const struct vf_params *p, float w, float h, float *ox, float *oy, float *cw, float *ch)
{
	crop_axis(w, p->crop[VF_CROP_L], p->crop[VF_CROP_R], ox, cw);
	crop_axis(h, p->crop[VF_CROP_T], p->crop[VF_CROP_B], oy, ch);
}

void vf_shape_rect(float cw, float ch, const int alt[4], float clip_w, float clip_h, float out[4])
{
	const float w = clip_w > 0.0f ? fminf(cw, clip_w) : cw;
	const float h = clip_h > 0.0f ? fminf(ch, clip_h) : ch;
	out[0] = (float)alt[0];
	out[1] = (float)alt[1];
	out[2] = w - (float)alt[2];
	out[3] = h - (float)alt[3];
}
