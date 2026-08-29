#!/bin/sh
#
# Run make targets in the bootstrapped OpenZFS userland build.
#
# Usage:
#   ./bootstrap/zfs-make.sh [make-target ...]
#   ./bootstrap/zfs-make.sh -C libnvpair all

set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "${SCRIPTDIR}/.." && pwd)"

BMAKE="${BMAKE:-${SCRIPTDIR}/tools/bin/bmake}"
TOOLDIR="${TOOLDIR:-${SCRIPTDIR}/tools}"
DESTDIR="${DESTDIR:-${SCRIPTDIR}/dest}"
OBJROOT="${OBJROOT:-${SCRIPTDIR}/obj-host}"

MACHINE="${MACHINE:-amd64}"
MACHINE_ARCH="${MACHINE_ARCH:-amd64}"
MACHINE_CPU="${MACHINE_CPU:-${MACHINE_ARCH}}"
JOBS="${JOBS:-16}"

CROSS_BINDIR="${SCRIPTDIR}/userland-tools/bin"
CROSS_CC="${CC:-${CROSS_BINDIR}/openbsd-cross-cc}"
CROSS_CXX="${CXX:-${CROSS_BINDIR}/openbsd-cross-c++}"
CROSS_INSTALL="${INSTALL:-${CROSS_BINDIR}/openbsd-install}"
ZFS_DIR="gnu/usr.sbin/zfs"

if [ "${1:-}" = -C ]; then
	[ "$#" -ge 2 ] || { echo "-C requires a subdirectory" >&2; exit 1; }
	ZFS_DIR="${ZFS_DIR}/$2"
	shift 2
fi

for tool in "${BMAKE}" "${CROSS_CC}" "${CROSS_CXX}" "${CROSS_INSTALL}"; do
	if [ ! -x "${tool}" ]; then
		echo "missing required executable: ${tool}" >&2
		echo "run ./bootstrap/cross-build-userland.sh once to create the cross-build environment" >&2
		exit 1
	fi
done

if [ "$#" -eq 0 ]; then
	set -- all
fi

BUILDUSER="${BUILDUSER:-$(id -un)}"
BUILDGROUP="${BUILDGROUP:-$(id -gn)}"

export PATH="${CROSS_BINDIR}:${TOOLDIR}/bin:${SCRIPTDIR}/wrap:${PATH}"

exec env \
    BSDSRCDIR="${SRCDIR}" \
    BSDOBJDIR="${OBJROOT}" \
    MAKEOBJDIR="obj.linux.${MACHINE}" \
    CROSSDIR="${DESTDIR}" \
    DESTDIR="${DESTDIR}" \
    MACHINE="${MACHINE}" \
    MACHINE_ARCH="${MACHINE_ARCH}" \
    MACHINE_CPU="${MACHINE_CPU}" \
    COMPILER_VERSION=clang \
    LINKER_VERSION=lld \
    WARNINGS=no \
    NOMAN=1 \
    NOPROFILE=1 \
    MAKECONF=/dev/null \
    MAKE="${BMAKE}" \
    CC="${CROSS_CC}" \
    CXX="${CROSS_CXX}" \
    LD="${TOOLDIR}/bin/ld.lld" \
    AR="${AR:-ar}" \
    RANLIB="${RANLIB:-ranlib}" \
    LORDER="${SCRIPTDIR}/wrap/lorder" \
    INSTALL="${CROSS_INSTALL}" \
    BINOWN="${BUILDUSER}" \
    BINGRP="${BUILDGROUP}" \
    LIBOWN="${BUILDUSER}" \
    LIBGRP="${BUILDGROUP}" \
    "${BMAKE}" -m "${SRCDIR}/share/mk" -j "${JOBS}" \
    -C "${SRCDIR}/${ZFS_DIR}" "$@"
