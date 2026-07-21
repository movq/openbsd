/*	$OpenBSD$	*/

/*
 * Copyright (c) 2026 Mike Jones <mike@mjones.org>
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
#include <sys/param.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <dev/biovar.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <readpassphrase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

#include "veracrypt.h"

static int	backing_open(const char *, int *, dev_t *, uint64_t *);
static int	bio_open(void **);
static int	bio_status(const struct bio_status *);
static void	usage(void);

static int
backing_open(const char *name, int *fdp, dev_t *devp, uint64_t *sizep)
{
	struct disklabel dl;
	struct stat st, bst;
	char *real;
	char shortname[PATH_MAX];
	uint64_t sectors;
	int bfd = -1, fd = -1, part;

	if ((fd = opendev(name, O_RDONLY, OPENDEV_PART, &real)) == -1)
		goto fail;
	if (fstat(fd, &st) == -1)
		goto fail;
	if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) {
		errno = ENODEV;
		goto fail;
	}

	if (S_ISBLK(st.st_mode)) {
		bst = st;
	} else {
		if (strncmp(real, "/dev/r", 6) != 0) {
			errno = EINVAL;
			goto fail;
		}
		if (strlcpy(shortname, real + 6, sizeof(shortname)) >=
		    sizeof(shortname)) {
			errno = ENAMETOOLONG;
			goto fail;
		}
		if ((bfd = opendev(shortname, O_RDONLY,
		    OPENDEV_PART | OPENDEV_BLCK, &real)) == -1)
			goto fail;
		if (fstat(bfd, &bst) == -1 || !S_ISBLK(bst.st_mode)) {
			errno = ENOTBLK;
			goto fail;
		}
	}

	if (ioctl(fd, DIOCGDINFO, &dl) == -1)
		goto fail;
	part = DISKPART(st.st_rdev);
	if (part >= dl.d_npartitions || dl.d_secsize != DEV_BSIZE) {
		errno = EINVAL;
		goto fail;
	}
	sectors = DL_GETPSIZE(&dl.d_partitions[part]);
	if (sectors > UINT64_MAX / dl.d_secsize) {
		errno = EOVERFLOW;
		goto fail;
	}
	*sizep = sectors * dl.d_secsize;
	*devp = bst.st_rdev;
	*fdp = fd;
	if (bfd != -1)
		close(bfd);
	return (0);

fail:
	if (bfd != -1)
		close(bfd);
	if (fd != -1)
		close(fd);
	return (-1);
}

static int
bio_open(void **cookiep)
{
	struct bio_locate bl;
	int fd;

	if ((fd = open("/dev/bio", O_RDWR)) == -1)
		return (-1);
	memset(&bl, 0, sizeof(bl));
	bl.bl_name = "softraid0";
	if (ioctl(fd, BIOCLOCATE, &bl) == -1) {
		close(fd);
		return (-1);
	}
	*cookiep = bl.bl_bio.bio_cookie;
	return (fd);
}

static int
bio_status(const struct bio_status *bs)
{
	const char *prefix;
	int i;

	prefix = bs->bs_controller[0] != '\0' ?
	    bs->bs_controller : "cryptctl";
	for (i = 0; i < bs->bs_msg_count; i++)
		fprintf(bs->bs_msgs[i].bm_type == BIO_MSG_INFO ?
		    stdout : stderr, "%s: %s\n", prefix,
		    bs->bs_msgs[i].bm_msg);

	return (bs->bs_status == BIO_STATUS_ERROR ? -1 : 0);
}

int
main(int argc, char *argv[])
{
	struct bioc_crypto_plain bcp;
	struct vc_mapping mapping;
	uint8_t volume_key[VC_VOLUME_KEY_SIZE];
	char passphrase[VC_PASSWORD_MAX + 1], why[160];
	const char *device, *errstr;
	dev_t backing_dev = NODEV;
	void *bio_cookie;
	uint64_t device_size;
	long long pim = 0;
	int backing_fd = -1, biofd = -1, ch, error = 1;

	while ((ch = getopt(argc, argv, "p:")) != -1) {
		switch (ch) {
		case 'p':
			pim = strtonum(optarg, 0, VC_PIM_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "PIM is %s: %s", errstr, optarg);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 1)
		usage();
	device = argv[0];

	if (backing_open(device, &backing_fd, &backing_dev,
	    &device_size) == -1)
		err(1, "%s", device);
	if ((biofd = bio_open(&bio_cookie)) == -1)
		err(1, "softraid0");

	if (readpassphrase("Passphrase: ", passphrase, sizeof(passphrase),
	    RPP_REQUIRE_TTY) == NULL) {
		warn("readpassphrase");
		goto out;
	}
#if 0
	if (pledge("stdio rpath wpath disklabel", NULL) == -1)
		err(1, "pledge");
#endif

	if (vc_read_unlock(backing_fd, device_size, passphrase,
	    strlen(passphrase), pim, &mapping, volume_key, why,
	    sizeof(why)) == -1) {
		warnx("%s", why);
		goto out;
	}
	explicit_bzero(passphrase, sizeof(passphrase));

	memset(&bcp, 0, sizeof(bcp));
	bcp.bcp_bio.bio_cookie = bio_cookie;
	bcp.bcp_backing_dev = backing_dev;
	bcp.bcp_data_offset = mapping.data_offset;
	bcp.bcp_data_length = mapping.data_length;
	bcp.bcp_iv_offset = mapping.iv_offset;
	memcpy(bcp.bcp_key, volume_key, sizeof(bcp.bcp_key));
	if (ioctl(biofd, BIOCCRYPTOPLAIN, &bcp) == -1) {
		warn("BIOCCRYPTOPLAIN");
		goto out;
	}
	if (bio_status(&bcp.bcp_bio.bio_status) == -1)
		goto out;
	error = 0;

out:
	explicit_bzero(passphrase, sizeof(passphrase));
	explicit_bzero(volume_key, sizeof(volume_key));
	explicit_bzero(&bcp, sizeof(bcp));
	explicit_bzero(&mapping, sizeof(mapping));
	if (backing_fd != -1)
		close(backing_fd);
	if (biofd != -1)
		close(biofd);
	return (error);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: cryptctl [-p pim] device\n");
	exit(1);
}
