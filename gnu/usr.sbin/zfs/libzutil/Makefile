#	$OpenBSD$

.include "../Makefile.inc"

LIB=	zutil
SRCS=	zutil_device_path.c zutil_import.c zutil_nicenum.c zutil_pool.c \
	zutil_device_path_os.c zutil_import_os.c

.PATH:	${ZFS_SRCS}/lib/libzutil \
	${ZFS_SRCS}/lib/libzutil/os/openbsd

install:

.include <bsd.lib.mk>
