#	$OpenBSD$

.include "../Makefile.inc"

LIB=	spl

SRCS=	assert.c backtrace.c atomic.c getexecname.c getexecname_os.c \
	gethostid_os.c getmntany.c list.c mkdirp.c mnttab.c page.c strlcat.c \
	strlcpy.c timestamp.c tunables.c zone_os.c

.PATH:	${ZFS_SRCS}/lib/libspl \
	${ZFS_SRCS}/lib/libspl/os/openbsd \
	${ZFS_SRCS}/lib/libspl/os/freebsd

install:

.include <bsd.lib.mk>
