/*	$OpenBSD$	*/

#ifndef _OPENBSD_ZFS_CONFIG_H_
#define _OPENBSD_ZFS_CONFIG_H_

#define ZFS_META_NAME		"zfs"
#define ZFS_META_VERSION	"2.4.3"
#define ZFS_META_RELEASE	"1"
#define ZFS_META_ALIAS		"zfs-2.4.3-1"
#define ZFS_META_LICENSE	"CDDL"
#define ZFS_META_AUTHOR		"OpenZFS"

#ifndef ETIME
#define ETIME		ETIMEDOUT
#endif

#define HAVE_INTTYPES		1
#define HAVE_ISSETUGID		1
#define HAVE_STRLCAT		1
#define HAVE_STRLCPY		1

/* OpenBSD has no native equivalents for these ZFS ioctl errors. */
#define ECKSUM			1053	/* ZFS_ERR_CKSUM */
#define ENOTACTIVE		1054	/* ZFS_ERR_NOTACTIVE */
#define EREMOTEIO		1055	/* ZFS_ERR_ACTIVE_POOL */

/* Linux/Solaris stream errors which have no OpenBSD errno value. */
#define ENOLINK			EPROTO
#define ENOSTR			ENOTCONN
#define ENODATA			EINVAL

#define LIBFETCH_DYNAMIC	0
#define LIBFETCH_IS_FETCH	0
#define LIBFETCH_IS_LIBCURL	0

#define dirent64		dirent
#define readdir64		readdir
#define pread64			pread
#define pwrite64		pwrite
#define statfs64		statfs

#endif /* _OPENBSD_ZFS_CONFIG_H_ */
