/*	$OpenBSD$	*/

/*
 * Copyright (c) 2026 Mike
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/dkio.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint64_t	parse_range(const char *, const char *);
static void	usage(void);

int
main(int argc, char *argv[])
{
	struct dk_discard discard = { 0 };
	u_int i;
	int fd;

	if (argc < 4 || (argc - 2) % 2 != 0 ||
	    (argc - 2) / 2 > DK_DISCARD_MAX_RANGES)
		usage();

	fd = open(argv[1], O_WRONLY);
	if (fd == -1)
		err(1, "%s", argv[1]);

	if (pledge("stdio disklabel", NULL) == -1)
		err(1, "pledge");

	discard.nranges = (argc - 2) / 2;
	for (i = 0; i < discard.nranges; i++) {
		discard.ranges[i].offset =
		    parse_range(argv[2 + i * 2], "offset");
		discard.ranges[i].length =
		    parse_range(argv[3 + i * 2], "length");
		if (discard.ranges[i].length == 0)
			errx(1, "length must be non-zero");
	}

	if (ioctl(fd, DIOCDISCARD, &discard) == -1)
		err(1, "DIOCDISCARD");

	if (close(fd) == -1)
		err(1, "close");
	return 0;
}

static uint64_t
parse_range(const char *value, const char *name)
{
	char *end;
	unsigned long long number;

	if (*value == '\0' || *value == '-')
		errx(1, "invalid %s: %s", name, value);

	errno = 0;
	number = strtoull(value, &end, 0);
	if (errno == ERANGE || *end != '\0')
		errx(1, "invalid %s: %s", name, value);

	return number;
}

static void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s device offset length [offset length ...]\n",
	    __progname);
	exit(1);
}
