PROG=	btrfs
BINDIR=	/sbin
MAN=	btrfs.8

.PATH:	${.CURDIR}/..

SRCS=	btrfs.c inspect.c inspect-dump-super.c inspect-dump-tree.c \
	inspect-tree-stats.c stubs.c subvolume-openbsd.c \
	stream-openbsd.c send-openbsd.c receive-openbsd.c

CPPFLAGS+=	-DBTRFS_FLAT_INCLUDES -I${.CURDIR}/.. \
	-I${.CURDIR}/../include -include ${.CURDIR}/../include/config.h
CFLAGS+=	-std=gnu11 -fno-strict-aliasing -pthread
LDADD+=	-L${.CURDIR}/../lib/libbtrfscommon \
	-L${.CURDIR}/../lib/libbtrfskernel \
	-lbtrfscommon -lbtrfskernel -pthread
DPADD+=	${.CURDIR}/../lib/libbtrfscommon/libbtrfscommon.a \
	${.CURDIR}/../lib/libbtrfskernel/libbtrfskernel.a

.include <bsd.prog.mk>
