#	$OpenBSD$

.include "../Makefile.prog.inc"

PROG=	zpool
SRCS=	zpool_iter.c zpool_main.c zpool_util.c zpool_vdev.c \
		zpool_vdev_os.c

.PATH:	${ZFS_SRCS}/cmd/zpool \
	${ZFS_SRCS}/cmd/zpool/os/openbsd

.include <bsd.prog.mk>
