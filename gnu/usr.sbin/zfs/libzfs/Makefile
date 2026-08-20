#	$OpenBSD$

.include "../Makefile.inc"

LIB=	zfs
SRCS=	libzfs_changelist.c libzfs_config.c libzfs_crypto.c \
	libzfs_dataset.c libzfs_diff.c libzfs_import.c libzfs_iter.c \
	libzfs_mount.c libzfs_pool.c libzfs_sendrecv.c libzfs_status.c \
	libzfs_util.c libzfs_compat_os.c libzfs_mount_os.c \
	cityhash.c zfeature_common.c zfs_comutil.c zfs_deleg.c \
	zfs_fletcher.c zfs_fletcher_aarch64_neon.c zfs_fletcher_avx512.c \
	zfs_fletcher_intel.c zfs_fletcher_sse.c \
	zfs_fletcher_superscalar.c zfs_fletcher_superscalar4.c \
	zfs_namecheck.c zfs_prop.c zfs_valstr.c zpool_prop.c zprop_common.c

.PATH:	${ZFS_SRCS}/lib/libzfs \
	${ZFS_SRCS}/lib/libzfs/os/openbsd \
	${ZFS_SRCS}/module/zcommon

install:

.include <bsd.lib.mk>
