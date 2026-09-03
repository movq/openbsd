PROG=	mount_btrfs
SRCS=	mount_btrfs.c getmntopts.c
MAN=

MOUNT=	${.CURDIR}/../mount
CFLAGS+= -I${MOUNT}
.PATH:	${MOUNT}

.include <bsd.prog.mk>
