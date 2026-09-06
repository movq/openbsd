LIB=	btrfscommon
NOPIC=
NOPROFILE=
NOMAN=

.PATH:	${.CURDIR}/../../common
.PATH:	${.CURDIR}/../../crypto

SRCS=	array.c compat.c cpu-utils.c device-utils.c extent-cache.c \
	extent-tree-utils.c root-tree-utils.c fsfeatures.c help.c \
	inject-error.c messages.c openbsd.c openbsd-stubs.c parse-utils.c path-utils.c \
	rbtree-utils.c sort-utils.c string-table.c string-utils.c units.c \
	utils.c uuid.c crc32c.c hash.c xxhash.c blake2b-ref.c sha224-256.c

CPPFLAGS+=	-DBTRFS_FLAT_INCLUDES -I${.CURDIR}/../.. \
	-I${.CURDIR}/../../include -include ${.CURDIR}/../../include/config.h
CFLAGS+=	-std=gnu11 -fno-strict-aliasing -pthread

.include <bsd.lib.mk>
