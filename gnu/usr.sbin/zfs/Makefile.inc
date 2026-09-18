#	$OpenBSD$

.if !defined(_ZFS_MAKEFILE_INC_)
_ZFS_MAKEFILE_INC_=1

ZFS_SRCS?=	${.CURDIR}/../../../openzfs
BINDIR?=	/usr/sbin

CPPFLAGS+=	-include ${.CURDIR}/../zfs_config.h
CPPFLAGS+=	-I${ZFS_SRCS}/lib/libspl/include/os/openbsd
# Reuse the portions of the BSD userland compatibility layer which do not
# need an OpenBSD-specific implementation.
CPPFLAGS+=	-I${ZFS_SRCS}/lib/libspl/include/os/freebsd
CPPFLAGS+=	-I${ZFS_SRCS}/lib/libspl/include
CPPFLAGS+=	-I${ZFS_SRCS}/include
CPPFLAGS+=	-I${ZFS_SRCS}/include/os/openbsd/zfs
CPPFLAGS+=	-I${ZFS_SRCS}/lib/libzutil
CPPFLAGS+=	-I${ZFS_SRCS}/lib/libzfs
CPPFLAGS+=	-I${ZFS_SRCS}/lib/libzfs_core
CPPFLAGS+=	-I${ZFS_SRCS}/lib/libshare
CPPFLAGS+=	-I${ZFS_SRCS}/lib/libtpool
CPPFLAGS+=	-I${ZFS_SRCS}/cmd/zpool
CPPFLAGS+=	-I${ZFS_SRCS}/cmd/zfs
CPPFLAGS+=	-D_REENTRANT
CPPFLAGS+=	-DLIBEXECDIR=\"/usr/libexec\"
CPPFLAGS+=	-DZFSEXECDIR=\"/usr/libexec/zfs\"
CPPFLAGS+=	-DRUNSTATEDIR=\"/var/run\"
CPPFLAGS+=	-DSBINDIR=\"/usr/sbin\"
CPPFLAGS+=	-DSYSCONFDIR=\"/etc\"
CPPFLAGS+=	-DPKGDATADIR=\"/usr/share/zfs\"
CPPFLAGS+=	-DTEXT_DOMAIN=\"zfs-openbsd-user\"

CFLAGS+=	-Wno-unused-parameter

NOPROFILE=

.endif
