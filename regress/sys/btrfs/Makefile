#	$OpenBSD$

PROG=	ref_decode
BTRFS_SYS=	${.CURDIR}/../../../sys
CPPFLAGS+=	-idirafter ${BTRFS_SYS}
LDFLAGS+=	-Wl,--gc-sections
LDADD+=		btrfs_ref.o
DPADD+=		btrfs_ref.o
CLEANFILES+=	btrfs_ref.o
REGRESS_TARGETS=	run-regress-${PROG} run-tree-layout
CLEANFILES+=	tree_layout btrfs_extent_buffer.o

run-tree-layout: tree_layout
	./tree_layout

tree_layout: tree_layout.c ${BTRFS_SYS}/btrfs/btrfs_tree.c \
    ${BTRFS_SYS}/btrfs/btrfs_var.h ${BTRFS_SYS}/btrfs/btrfs.h \
    btrfs_extent_buffer.o
	${CC} ${CFLAGS} -D_KERNEL -ffunction-sections -fdata-sections \
	    -I${BTRFS_SYS} -I${BTRFS_SYS}/arch \
	    -Wl,--gc-sections ${.CURDIR}/tree_layout.c \
	    btrfs_extent_buffer.o -o ${.TARGET}

btrfs_extent_buffer.o: ${BTRFS_SYS}/btrfs/btrfs_extent_buffer.c \
    ${BTRFS_SYS}/btrfs/btrfs_var.h ${BTRFS_SYS}/btrfs/btrfs.h
	${CC} ${CFLAGS} -D_KERNEL -ffunction-sections -fdata-sections \
	    -I${BTRFS_SYS} -I${BTRFS_SYS}/arch \
	    -c ${BTRFS_SYS}/btrfs/btrfs_extent_buffer.c -o ${.TARGET}

# Link the real decoder; discard the kernel-only transaction/edit functions.
btrfs_ref.o: ${BTRFS_SYS}/btrfs/btrfs_ref.c \
    ${BTRFS_SYS}/btrfs/btrfs_ref.h ${BTRFS_SYS}/btrfs/btrfs_var.h \
    ${BTRFS_SYS}/btrfs/btrfs.h
	${CC} ${CFLAGS} -D_KERNEL -ffunction-sections -fdata-sections \
	    -I${BTRFS_SYS} -I${BTRFS_SYS}/arch \
	    -c ${BTRFS_SYS}/btrfs/btrfs_ref.c -o ${.TARGET}

.include <bsd.regress.mk>
