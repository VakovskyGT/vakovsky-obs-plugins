/* Browser ReSize: pure decision of which scene's size a source should have now. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A switch of the source's size from (from_w x from_h) to (to_w x to_h) during a transition. */
struct vf_size_anim {
	uint64_t start_ns;
	uint32_t ms;
	long long from_w, from_h, to_w, to_h;
	int easing, easing_fn; /* Move's matched-item curve; easing_fn <= 0 keeps the usual smooth curve. */
};

/* The frame (the size OBS sees, Ultimate Frame's effects follow it) moves smoothly from the old size to the new one,
 * eased like Ultimate Frame's animation; mix is that eased progress and page
 * the size the page ends at. Returns false once the switch is over (box and page are then the new size, mix 1). */
bool vf_size_anim_at(const struct vf_size_anim *a, uint64_t now_ns, float *box_w, float *box_h, long long *page_w,
		     long long *page_h, float *mix);

/* Where a new switch starts. While a switch or its hold is still on screen (busy), from the box the viewer sees
 * now, so a switch mid-transition does not jump; otherwise from the old scene's stored size (have_stored), or the
 * source's current size when that scene has none. */
void vf_size_pick_from(bool busy, float box_w, float box_h, bool have_stored, long long stored_w, long long stored_h,
		       long long cur_w, long long cur_h, long long *from_w, long long *from_h);

/* Whether the source must be updated to reach w x h: its settings or its real size (they can disagree after a
 * properties dialog's Cancel) is something else. */
bool vf_size_needs_update(long long set_w, long long set_h, long long real_w, long long real_h, long long w,
			  long long h);

/* Opacity of the last good frame over the live page once the page has redrawn (end_ns): it fades out over fade_ns
 * instead of vanishing in one frame. 0 when fade_ns is 0. */
float vf_size_fade_alpha(uint64_t end_ns, uint64_t now_ns, uint64_t fade_ns);

enum vf_size_switch {
	VF_SW_NONE,    /* the new scene has no size for the source: leave the page alone */
	VF_SW_KEEP,    /* the source is leaving: freeze the frame as it is, Move has already taken its size */
	VF_SW_JUMP,    /* appearing, or a Cut: straight to the new size */
	VF_SW_ANIMATE, /* in both scenes: the frame moves to the new size during the transition */
};

/* What a program switch does, by whether the source is in the old and the new scene and the new one has a size.
 * smooth off: the size changes at once even in a timed transition. */
enum vf_size_switch vf_size_switch_kind(bool in_from, bool in_program, bool has_size, uint32_t ms, bool smooth);

bool vf_size_anchor(int mode, float *x, float *y);

/* Hold the transition workspace height only during motion; idle and Cut use the exact scene height. */
uint32_t vf_size_page_height(bool fixed, bool moving, uint32_t requested, uint32_t working_height);
/* How long a new page size settles before it is captured: a discrete Fade must not freeze the first old-layout
 * frame, and the browser takes more than a frame to redraw at a new height. */
uint64_t vf_size_settle_ns(bool dissolve, bool fixed_height, uint32_t new_h, uint32_t cur_h, uint64_t frame_ns);

enum vf_size_fade_role { VF_SIZE_FADE_LIVE, VF_SIZE_FADE_OLD, VF_SIZE_FADE_NEW };
enum vf_size_fade_role vf_size_fade_role(bool active, const char *render_scene, const char *from, const char *to);

/* A scene's page size; 0 in a field keeps the page's current value there. False when both are 0 (no size). */
bool vf_size_resolve(long long w, long long h, uint32_t cur_w, uint32_t cur_h, long long *out_w, long long *out_h);

/* A page-space length (profile, frame) as the picture the filter gets: filters below it (a crop) change the page's
 * size by a fixed amount, page - image. At least 1. */
long vf_size_to_image(long page_len, uint32_t page, uint32_t image);

/* A per-scene size key, "w_<uuid>" or "h_<uuid>": either one alone makes a profile (the other field is then 0).
 * Copies the uuid; false for other keys or a uuid that does not fit in uuid_len. */
bool vf_size_profile_key(const char *name, size_t uuid_len, char *uuid);

/* A per-scene size key ("w_<uuid>" / "h_<uuid>") of a scene that is not among the known ones: drop it. */
bool vf_size_stale_key(const char *name, const char *const *known_uuids, size_t n);

/* Which scene the properties dialog shows (index into n ids of stride bytes each): a newly opened dialog (fresh)
 * starts on the current scene, an open one keeps the stored choice; a choice that is gone falls back to the current
 * scene, then to the first. 0 when n is 0. */
size_t vf_size_edit_pick(const char *stored, const char *current, bool fresh, const char *ids, size_t stride, size_t n);

/* The scene the capture button saves the size for: the one picked in the dialog's list, or program without a pick. */
const char *vf_size_capture_scene(const char *picked, const char *program);
