#include "vf-resize.h"

#include <string.h>

void vf_resize_reset(struct vf_resize *s, uint64_t epoch, uint64_t settle_ns)
{
	memset(s, 0, sizeof(*s));
	s->tag.epoch = epoch;
	s->settle_ns = settle_ns;
	s->latency_ns = settle_ns * 2;
}

bool vf_resize_current(const struct vf_resize *s, struct vf_resize_tag tag)
{
	return s->phase != VF_RESIZE_IDLE && tag.epoch == s->tag.epoch && tag.id == s->tag.id && tag.w == s->tag.w &&
	       tag.h == s->tag.h;
}

bool vf_resize_issue(struct vf_resize *s, uint32_t w, uint32_t h, uint64_t now_ns, bool urgent)
{
	if (!w || !h)
		return false;
	if (s->phase != VF_RESIZE_IDLE) {
		if (!urgent || (s->tag.w == w && s->tag.h == h))
			return false;
	} else if (s->have_accepted && s->accepted_w == w && s->accepted_h == h) {
		return false;
	}
	s->tag.id++;
	s->tag.w = w;
	s->tag.h = h;
	s->asked_ns = now_ns;
	s->phase = VF_RESIZE_QUEUED;
	return true;
}

bool vf_resize_applied(struct vf_resize *s, struct vf_resize_tag tag, uint64_t now_ns, uint64_t frame)
{
	if (!vf_resize_current(s, tag) || s->phase != VF_RESIZE_QUEUED)
		return false;
	s->applied_ns = now_ns;
	s->applied_frame = frame;
	s->phase = VF_RESIZE_WAIT;
	return true;
}

bool vf_resize_sample(struct vf_resize *s, uint64_t now_ns, uint64_t frame, uint32_t w, uint32_t h)
{
	if (s->phase != VF_RESIZE_WAIT || frame <= s->applied_frame || w != s->tag.w || h != s->tag.h ||
	    now_ns < s->applied_ns || now_ns - s->applied_ns < s->settle_ns)
		return false;
	s->sampled_frame = frame;
	s->phase = VF_RESIZE_CHECK;
	return true;
}

enum vf_resize_result vf_resize_complete(struct vf_resize *s, struct vf_resize_tag tag, uint64_t frame, uint64_t now_ns,
					 bool good)
{
	if (!vf_resize_current(s, tag) || s->phase != VF_RESIZE_CHECK || frame <= s->sampled_frame)
		return VF_RESIZE_STALE;
	if (!good) {
		s->phase = VF_RESIZE_WAIT;
		return VF_RESIZE_REJECTED;
	}
	uint64_t elapsed = now_ns > s->applied_ns ? now_ns - s->applied_ns : 0;
	/* A long stall must not make the prediction jump past every later transition. */
	if (elapsed > 500000000ULL)
		elapsed = 500000000ULL;
	s->latency_ns = (s->latency_ns * 3 + elapsed) / 4;
	s->accepted_w = tag.w;
	s->accepted_h = tag.h;
	s->have_accepted = true;
	s->retries = 0;
	s->phase = VF_RESIZE_IDLE;
	return VF_RESIZE_ACCEPTED;
}

bool vf_resize_expired(const struct vf_resize *s, uint64_t now_ns, uint64_t timeout_ns)
{
	return s->phase != VF_RESIZE_IDLE && now_ns >= s->asked_ns && now_ns - s->asked_ns >= timeout_ns;
}

/* A new size makes the browser lay out and draw again; the stuck one may never get a good frame on a static page. */
bool vf_resize_retry(struct vf_resize *s, uint32_t w, uint32_t h, uint64_t now_ns, uint32_t max_retries)
{
	if (!w || !h || s->retries >= max_retries)
		return false;
	s->retries++;
	s->tag.id++;
	s->tag.w = w;
	s->tag.h = h;
	s->asked_ns = now_ns;
	s->phase = VF_RESIZE_QUEUED;
	return true;
}

float vf_resize_blend(uint64_t start_ns, uint64_t now_ns, uint64_t frame_interval_ns)
{
	if (!frame_interval_ns)
		return 1.0f;
	if (now_ns <= start_ns)
		return 0.0f;
	uint64_t elapsed = now_ns - start_ns;
	return elapsed >= frame_interval_ns ? 1.0f : (float)((double)elapsed / (double)frame_interval_ns);
}
