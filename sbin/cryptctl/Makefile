#	$OpenBSD$

PROG=	cryptctl
SRCS=	cryptctl.c veracrypt.c

CDIAGFLAGS+=	-Wall

LDADD=	-lcrypto -lutil
DPADD=	${LIBCRYPTO} ${LIBUTIL}

MAN=	cryptctl.8

.include <bsd.prog.mk>
