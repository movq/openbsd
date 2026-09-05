PROG=	mount_btrfs
SRCS=	mount_btrfs.c getmntopts.c
MAN=	mount_btrfs.8

MOUNT=	${.CURDIR}/../mount
CFLAGS+= -I${MOUNT}
.PATH:	${MOUNT}

.include <bsd.prog.mk>
