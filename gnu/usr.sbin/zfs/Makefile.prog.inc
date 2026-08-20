#	$OpenBSD$

.include "Makefile.inc"

# The OpenZFS libraries are private static archives for now.  Keep them in a
# linker group because there are intentional cycles between the compatibility,
# nvlist, import, sharing, and public API layers.
ZFS_LIBS=	libzfs libzfs_core libshare libzutil libuutil libnvpair \
		libtpool libavl libspl

LDADD+=		-Wl,--start-group
.for lib in ${ZFS_LIBS}
DPADD+=		${.OBJDIR}/../${lib}/${lib}.a
LDADD+=		${.OBJDIR}/../${lib}/${lib}.a
.endfor
LDADD+=		-Wl,--end-group -lcrypto -lz -lm -lpthread

NOMAN=
