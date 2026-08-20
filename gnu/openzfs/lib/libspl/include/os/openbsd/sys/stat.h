// SPDX-License-Identifier: CDDL-1.0

#ifndef _LIBSPL_OPENBSD_SYS_STAT_H
#define _LIBSPL_OPENBSD_SYS_STAT_H

#include_next <sys/stat.h>
#include <sys/disklabel.h>
#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <stdint.h>

#ifndef DIOCGDINFO
#define DIOCGDINFO	_IOR('d', 101, struct disklabel)
#endif

#define stat64	stat
#define lstat64	lstat
#define MAXOFFSET_T	INT64_MAX

static inline int
fstat64(int fd, struct stat *st)
{
	struct disklabel dl;
	uint64_t sectors;
	int ret;

	ret = fstat(fd, st);
	if (ret == 0 && (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) &&
	    ioctl(fd, DIOCGDINFO, &dl) == 0 && dl.d_secsize != 0) {
		sectors = DL_GETPSIZE(&dl.d_partitions[DISKPART(st->st_rdev)]);
		if (sectors <= INT64_MAX / dl.d_secsize)
			st->st_size = sectors * dl.d_secsize;
	}
	return (ret);
}

static inline int
fstat64_blk(int fd, struct stat64 *st)
{
	return (fstat64(fd, st));
}

#endif /* _LIBSPL_OPENBSD_SYS_STAT_H */
