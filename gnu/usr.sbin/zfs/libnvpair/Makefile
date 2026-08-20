#	$OpenBSD$

.include "../Makefile.inc"

LIB=	nvpair
SRCS=	libnvpair.c libnvpair_json.c nvpair_alloc_system.c \
	nvpair_alloc_fixed.c nvpair.c fnvpair.c

.PATH:	${ZFS_SRCS}/lib/libnvpair \
	${ZFS_SRCS}/module/nvpair

install:

.include <bsd.lib.mk>
