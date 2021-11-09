#	$OpenBSD: Makefile,v 1.11.2.1 2021/11/09 13:40:41 benno Exp $

PROG=	openrsync
SRCS=	blocks.c client.c copy.c downloader.c fargs.c flist.c hash.c ids.c \
	io.c log.c main.c misc.c mkpath.c mktemp.c receiver.c rmatch.c \
	rules.c sender.c server.c session.c socket.c symlinks.c uploader.c
LDADD+= -lcrypto -lm -lutil
DPADD+= ${LIBCRYPTO} ${LIBM} ${LIBUTIL}
MAN=	openrsync.1

CFLAGS+= -Wall -Wextra
CFLAGS+= -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+= -Wmissing-declarations
CFLAGS+= -Wshadow


openrsync.1: rsync.1
	ln -sf ${.CURDIR}/rsync.1 openrsync.1

CLEANFILES+=	openrsync.1

.include <bsd.prog.mk>
