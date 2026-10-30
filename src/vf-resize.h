/* Non-blocking browser resize requests. Callers serialize UI, tick and render access. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct vf_resize_tag {
	uint64_t epoch, id;
	uint32_t w, h;
};

enum vf_resize_phase { VF_RESIZE_IDLE, VF_RESIZE_QUEUED, VF_RESIZE_WAIT, VF_RESIZE_CHECK };
enum vf_resize_result { VF_RESIZE_STALE, VF_RESIZE_REJECTED, VF_RESIZE_ACCEPTED };

struct vf_resize {
	struct vf_resize_tag tag;
	enum vf_resize_phase phase;
	uint64_t asked_ns, applied_ns, applied_frame, sampled_frame, settle_ns, latency_ns;
	uint32_t accepted_w, accepted_h;
	uint32_t retries; /* stuck steps asked again since the last accepted one */
	bool have_accepted;
};

void vf_resize_reset(struct vf_resize *s, uint64_t epoch, uint64_t settle_ns);
/* Urgent: replace an intermediate request with the final size, or cancel it on a new transition. */
bool vf_resize_issue(struct vf_resize *s, uint32_t w, uint32_t h, uint64_t now_ns, bool urgent);
bool vf_resize_current(const struct vf_resize *s, struct vf_resize_tag tag);
bool vf_resize_applied(struct vf_resize *s, struct vf_resize_tag tag, uint64_t now_ns, uint64_t frame);
bool vf_resize_sample(struct vf_resize *s, uint64_t now_ns, uint64_t frame, uint32_t w, uint32_t h);
enum vf_resize_result vf_resize_complete(struct vf_resize *s, struct vf_resize_tag tag, uint64_t frame, uint64_t now_ns,
					 bool good);
bool vf_resize_expired(const struct vf_resize *s, uint64_t now_ns, uint64_t timeout_ns);
/* A step the browser never drew right is replaced by a fresh request at w x h; false once max_retries in a row were
 * used, and the caller gives up the steps. */
bool vf_resize_retry(struct vf_resize *s, uint32_t w, uint32_t h, uint64_t now_ns, uint32_t max_retries);
/* Snapshot blend completes within one video interval, before another checked snapshot can replace it. */
float vf_resize_blend(uint64_t start_ns, uint64_t now_ns, uint64_t frame_interval_ns);
