LIB=	btrfskernel
NOPIC=
NOPROFILE=
NOMAN=

.PATH:	${.CURDIR}/../../kernel-lib
.PATH:	${.CURDIR}/../../kernel-shared

SRCS=	list_sort.c raid56.c rbtree.c tables.c \
	accessors.c async-thread.c backref.c ctree.c delayed-ref.c \
	dir-item.c disk-io.c extent-io-tree.c extent-tree.c extent_io.c \
	file-item.c file.c free-space-cache.c free-space-tree.c inode-item.c \
	inode.c locking.c messages.c print-tree.c root-tree.c transaction.c \
	tree-checker.c ulist.c uuid-tree.c volumes.c zoned.c

CPPFLAGS+=	-DBTRFS_FLAT_INCLUDES -I${.CURDIR}/../.. \
	-I${.CURDIR}/../../include -include ${.CURDIR}/../../include/config.h
CFLAGS+=	-std=gnu11 -fno-strict-aliasing -pthread

.include <bsd.lib.mk>
