/* Identify which scene item copy of a source is being rendered, from the current render matrix. Pure. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VF_MATCH_EPS 1e-3f

struct vf_copy {
	float dt[16]; /* draw_transform, same layout as struct matrix4 (rows x, y, z, t) */
	int crop_l, crop_t, crop_r, crop_b;
	bool texpath; /* OBS renders the item through an intermediate texture */
	int rank;     /* lower wins on equal transforms, see vf_copy_rank */
	bool hidden;  /* not drawn (hidden item or group): never the copy being rendered */
};

enum vf_match_kind { VF_MATCH_NONE = 0, VF_MATCH_EXACT = 1, VF_MATCH_SCALED = 2 };

struct vf_match_result {
	int index;
	enum vf_match_kind kind;
	float err;
};

/* allow_scaled: also accept a copy drawn under an unknown uniform scale (window renders only: Studio Mode
 * preview, multiview); the output render is always at canvas scale. */
struct vf_match_result vf_match_copy(const float m[16], uint32_t src_w, uint32_t src_h, const struct vf_copy *copies,
				     size_t n, bool allow_scaled);
void vf_copy_scale(const struct vf_copy *c, float *sx, float *sy);
/* A copy Move draws through its own texture (a nested scene) has no scale in its render matrix: its canvas scale
 * goes from the copy in the scene it leaves to the one in the scene it enters, at t along Move's curve (clamped to
 * 0..1). A missing side takes the other's scale. */
void vf_copy_scale_moving(const struct vf_copy *from, const struct vf_copy *to, float t, float *sx, float *sy);

/* Tie-break rank of a copy in `scene` (lower wins). window_render: drawn into a window viewport (Studio Mode
 * preview), where the preview scene is preferred; otherwise (output render) the program scene. Next comes `from`,
 * the scene the program is switching away from: its copy is still drawn while it leaves. */
int vf_copy_rank(const char *scene, const char *program, const char *from, const char *preview, bool window_render);
/* During a transition the scene scope filter names the scene being drawn (scope): its copy wins over every other at
 * the same place, so each side of a Fade shows its own scene's look. scope NULL or "": rank unchanged. */
int vf_copy_rank_scoped(int rank, const char *scene, const char *scope);
