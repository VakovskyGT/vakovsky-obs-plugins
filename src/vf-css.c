#include "vf-css.h"

#include <stdlib.h>
#include <string.h>

/* A 1 px dot at 1% opacity that changes colour 30 times a second: invisible, but Chromium draws a frame each time.
 * html::after, not body::after: pages style body::after more often. */
static const char block[] = VF_CSS_BEGIN
	"\n"
	"html::after { content: \"\"; position: fixed; left: 50%; top: 50%; width: 1px; height: 1px;"
	" opacity: 0.01; pointer-events: none; animation: vaki-redraw 1s steps(30) infinite; }\n"
	"@keyframes vaki-redraw { from { background: #000; } to { background: #fff; } }\n" VF_CSS_END;

bool vf_css_has_redraw(const char *css)
{
	return css && strstr(css, VF_CSS_BEGIN) != NULL;
}

static char *join(const char *a, size_t an, const char *b, size_t bn, const char *c, size_t cn)
{
	char *s = malloc(an + bn + cn + 1);
	if (!s)
		return NULL;
	memcpy(s, a, an);
	memcpy(s + an, b, bn);
	memcpy(s + an + bn, c, cn);
	s[an + bn + cn] = '\0';
	return s;
}

char *vf_css_set_redraw(const char *css, bool on)
{
	if (!css)
		css = "";
	const char *begin = strstr(css, VF_CSS_BEGIN);
	if (on) {
		if (begin)
			return NULL;
		const size_t n = strlen(css);
		const bool newline = n && css[n - 1] != '\n';
		return join(css, n, "\n", newline ? 1 : 0, block, strlen(block));
	}

	if (!begin)
		return NULL;
	const char *end = strstr(begin, VF_CSS_END);
	if (!end)
		return NULL; /* the end mark was edited away: cutting to a guess could take the user's CSS with it */
	end += strlen(VF_CSS_END);
	if (*end == '\n')
		end++;
	size_t keep = (size_t)(begin - css);
	if (!*end)
		while (keep && css[keep - 1] == '\n') /* the newline added in front of the block */
			keep--;
	return join(css, keep, end, strlen(end), "", 0);
}
