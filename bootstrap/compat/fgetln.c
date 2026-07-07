/*
 * fgetln(3) — BSD get a line from a stream.
 * Implemented using getline(3) with per-stream buffers.
 */
#define _GNU_SOURCE	/* for getline() */
#include <stdio.h>
#include <stdlib.h>

#define FGETLN_POOL_SIZE 16

struct fb_entry {
	FILE	*fp;
	char	*buf;
	size_t	 bufsz;
};

static struct fb_entry fb_pool[FGETLN_POOL_SIZE];
static int fb_pool_idx;

char *
fgetln(FILE *stream, size_t *len)
{
	struct fb_entry *fb;
	ssize_t n;

	flockfile(stream);

	fb = &fb_pool[fb_pool_idx];
	if (fb->fp != stream || fb->fp == NULL) {
		int i;

		for (i = 0; i < FGETLN_POOL_SIZE; i++) {
			if (fb_pool[i].fp == stream) {
				fb_pool_idx = i;
				fb = &fb_pool[i];
				goto found;
			}
		}
		fb_pool_idx = (fb_pool_idx + 1) % FGETLN_POOL_SIZE;
		fb = &fb_pool[fb_pool_idx];
	}

found:
	fb->fp = stream;
	n = getline(&fb->buf, &fb->bufsz, stream);
	funlockfile(stream);

	if (n == -1) {
		*len = 0;
		return NULL;
	}
	*len = (size_t)n;
	return fb->buf;
}
