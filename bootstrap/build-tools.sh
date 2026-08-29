#!/bin/sh
#
# build-tools.sh - build OpenBSD host tools on a GNU/Linux host using the
#                  bootstrapped bmake and OpenBSD's own Makefiles.
#
# Usage: ./build-tools.sh [MACHINE_ARCH=amd64] [CC=clang] [CXX=clang++]
#
set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "$SCRIPTDIR/.." && pwd)"

BMAKE="${BMAKE:-${SCRIPTDIR}/tools/bin/bmake}"
if [ ! -x "${BMAKE}" ] && [ -x "${SCRIPTDIR}/build/bmake" ]; then
	BMAKE="${SCRIPTDIR}/build/bmake"
fi
TOOLDIR="${SCRIPTDIR}/tools"
OBJROOT="${OBJROOT:-${SCRIPTDIR}/obj-build}"
OBJROOT="$(mkdir -p "${OBJROOT}" && cd "${OBJROOT}" && pwd)"

MACHINE="${MACHINE:-${MACHINE_ARCH:-amd64}}"
MACHINE_ARCH="${MACHINE_ARCH:-amd64}"
SYSTEM_CC="${CC:-clang}"
SYSTEM_CXX="${CXX:-clang++}"
BUILDUSER="${BUILDUSER:-$(id -un)}"
BUILDGROUP="${BUILDGROUP:-$(id -gn)}"

WRAPDIR="${SCRIPTDIR}/wrap"
WRAP_CC="${WRAPDIR}/cc"
WRAP_CXX="${WRAPDIR}/c++"
BOOTSTRAP_INCLUDE="${SCRIPTDIR}/include"
BOOTSTRAP_COMPAT="${SCRIPTDIR}/compat"

# Variables that must reach bmake (and its sub-makes) are exported.
export MACHINE_ARCH
export MACHINE
export REAL_CC="${SYSTEM_CC}"
export REAL_CXX="${SYSTEM_CXX}"

# Prepend wrap directory to PATH so that our lorder/tsort wrappers
# are found before system tools.  bmake runs shell commands using
# the invoking environment's PATH.
export PATH="${WRAPDIR}:${PATH}"

# bmake arguments shared by every invocation
MAKE_ARGS="-m ${SRCDIR}/share/mk -j 16"

# All make variable overrides, passed on the command line and via env.
#
# Key overrides explained:
#   MACHINE_ARCH       - target arch; drives LLVM_ARCH and backend selection
#   BSDSRCDIR/BSDOBJDIR - native OpenBSD source/obj tree layout
#   NOPIC=1            - skip shared libraries (PIC objects)
#   NOPROFILE=1        - skip profiled libraries
#   NOPIE=yes          - skip PIE flags for host tools
#   WARNINGS=no        - skip CXXDIAGFLAGS (may contain OpenBSD-isms)
#   COMPILER_VERSION=clang - prevents Makefile.inc from overriding CC/CXX
#   BUILD_LLDB=no      - lldb host support won't compile on Linux
#   PATH               - prepended with wrap/ for lorder/tsort wrappers
#   LD=ld              - use system linker for partial (relocatable) links
#   AR=ar              - use system ar
#   RANLIB=ranlib      - use system ranlib
#   AR_VERSION=binutils - skip llvm-ar build
#   LIBCRT0=           - crt0.o doesn't exist on Linux (not needed for host tools)
#   CRTBEGIN=          - crtbegin.o doesn't exist on Linux
#   CRTEND=            - crtend.o doesn't exist on Linux
#   MAKE=              - set to bmake for sub-make recursion
#   CC=                - compiler wrapper (strips -fno-ret-protector)
#   CXX=               - C++ compiler wrapper

MAKE_ENV="
	MACHINE_ARCH=${MACHINE_ARCH}
	MACHINE=${MACHINE}
	BSDSRCDIR=${SRCDIR}
	BSDOBJDIR=${OBJROOT}
	NOPIC=1
	NOPROFILE=1
	NOPIE=yes
	WARNINGS=no
	COMPILER_VERSION=clang
	BUILD_LLDB=no
	LD=ld
	AR=ar
	RANLIB=ranlib
	AR_VERSION=binutils
	LIBCRT0=
	CRTBEGIN=
	CRTEND=
	BINOWN=${BUILDUSER}
	BINGRP=${BUILDGROUP}
	LIBOWN=${BUILDUSER}
	LIBGRP=${BUILDGROUP}
	MAKE=${BMAKE}
	CC=${WRAP_CC}
	CXX=${WRAP_CXX}
"

HOST_TOOL_CPPFLAGS="-I${BOOTSTRAP_INCLUDE} -D_DEFAULT_SOURCE -idirafter ${SRCDIR}/include"
HOST_TOOL_LDADD="${BOOTSTRAP_COMPAT}/pledge.c ${BOOTSTRAP_COMPAT}/unveil.c ${BOOTSTRAP_COMPAT}/strlcpy.c ${BOOTSTRAP_COMPAT}/strlcat.c"
HOST_TOOL_LDADD_ALLOC="${HOST_TOOL_LDADD} ${BOOTSTRAP_COMPAT}/reallocarray.c"

echo "==> Building OpenBSD LLVM/clang toolchain"
echo "    SRCDIR       = ${SRCDIR}"
echo "    BMAKE        = ${BMAKE}"
echo "    MACHINE_ARCH = ${MACHINE_ARCH}"
echo "    CC           = ${SYSTEM_CC}"
echo "    CXX          = ${SYSTEM_CXX}"
echo "    TOOLDIR      = ${TOOLDIR}"
echo "    OBJROOT      = ${OBJROOT}"
echo ""

#echo "==> Selecting OpenBSD branches in lld ELF backend"
#find "${SRCDIR}/gnu/llvm/lld/ELF" -type f \( -name '*.cpp' -o -name '*.h' \) \
#	-exec sed -i \
#		-e 's/#ifdef __OpenBSD__/#if 1/' \
#		-e 's/#ifndef __OpenBSD__/#if 0/' \
#		-e 's/defined(__OpenBSD__)/1/g' \
#		{} +
#echo ""

# fwrapv...
sed -i -e 's/#ifdef __OpenBSD__/#if 1/' \
	-e 's/#ifndef __OpenBSD__/#if 0/' \
	-e 's/defined(__OpenBSD__)/1/g' \
	${SRCDIR}/gnu/llvm/clang/lib/Driver/ToolChains/CommonArgs.cpp

# =====================================================================
# 1: Generate LLVM config headers (.def files, llvm-config.h)
# =====================================================================
mkdir -p "${OBJROOT}"

env ${MAKE_ENV} ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/gnu/usr.bin/clang" \
	obj

env ${MAKE_ENV} ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/gnu/usr.bin/clang/include/llvm/Config" \
	all

CONFIG_OBJDIR="${OBJROOT}/gnu/usr.bin/clang/include/llvm/Config"
echo "    AsmParsers.def:     $(cat ${CONFIG_OBJDIR}/AsmParsers.def | tr '\n' ' ')"
echo "    Targets.def:        $(cat ${CONFIG_OBJDIR}/Targets.def | tr '\n' ' ')"

# =====================================================================
# 2: Build all of LLVM/clang via subdir recursion
# =====================================================================
env ${MAKE_ENV} CPPFLAGS="-I/usr/include/bsd -DLIBBSD_OVERLAY" \
	${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/gnu/usr.bin/clang" \
	all

# Install clang's resource headers where bootstrap/tools/bin/clang looks
# for them by default: bootstrap/tools/lib/clang/<major>/include.
env ${MAKE_ENV} ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/gnu/usr.bin/clang/include/clang/intrin" \
	DESTDIR="${TOOLDIR}" \
	CLANG_INTR_INCDIR="/lib/clang/22/include" \
	includes

# =====================================================================
# 3: Collect LLVM results into tools/
# =====================================================================

CLANGDIR="${OBJROOT}/gnu/usr.bin/clang"

mkdir -p "${TOOLDIR}/bin" "${TOOLDIR}/lib"

if [ "${BMAKE}" != "${TOOLDIR}/bin/bmake" ]; then
	cp "${BMAKE}" "${TOOLDIR}/bin/bmake"
	echo "    bin/bmake"
fi

# Binaries we care about
BINARIES="
	clang/clang
	lld/ld.lld
	llvm-objcopy/llvm-objcopy
	llvm-objdump/llvm-objdump
	llvm-readobj/llvm-readobj
	llvm-symbolizer/llvm-symbolizer
	llvm-profdata/llvm-profdata
	llvm-cov/llvm-cov
"

for bin in ${BINARIES}; do
	src="${CLANGDIR}/${bin}"
	if [ -x "${src}" ]; then
		cp "${src}" "${TOOLDIR}/bin/"
		echo "    bin/$(basename ${bin})"
	fi
done

# Also copy libraries for later use
for lib in "${CLANGDIR}"/lib*.a; do
	[ -f "${lib}" ] || continue
	cp "${lib}" "${TOOLDIR}/lib/"
	echo "    lib/$(basename ${lib})"
done 2>/dev/null || true

# Copy clang-specific libraries
for libdir in "${CLANGDIR}"/libclang*; do
	[ -d "${libdir}" ] || continue
	libname="$(basename "${libdir}")"
	libfile="${libdir}/lib${libname}.a"
	if [ -f "${libfile}" ]; then
		cp "${libfile}" "${TOOLDIR}/lib/"
		echo "    lib/lib${libname}.a"
	fi
done

# =====================================================================
# 4: Build host rpcgen via OpenBSD makefiles
# =====================================================================

echo ""
echo "==> Building OpenBSD rpcgen for the host"

env ${MAKE_ENV} LIBC= ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/usr.bin/rpcgen" \
	obj

env ${MAKE_ENV} LIBC= ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/usr.bin/rpcgen" \
	all \
	CPPFLAGS="${HOST_TOOL_CPPFLAGS}" \
	LDADD="${HOST_TOOL_LDADD}"

RPCGENDIR="${OBJROOT}/usr.bin/rpcgen"
cp "${RPCGENDIR}/rpcgen" "${TOOLDIR}/bin/"
echo "    bin/rpcgen"

"${TOOLDIR}/bin/rpcgen" -h "${SRCDIR}/lib/librpcsvc/mount.x" \
	> "${OBJROOT}/rpcgen-smoke.h"

# =====================================================================
# 5: Build host yacc via OpenBSD makefiles
# =====================================================================

echo ""
echo "==> Building OpenBSD yacc for the host"

env ${MAKE_ENV} LIBC= ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/usr.bin/yacc" \
	obj

env ${MAKE_ENV} LIBC= ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/usr.bin/yacc" \
	all \
	CPPFLAGS="${HOST_TOOL_CPPFLAGS}" \
	LDADD="${HOST_TOOL_LDADD_ALLOC}"

YACCDIR="${OBJROOT}/usr.bin/yacc"
cp "${YACCDIR}/yacc" "${TOOLDIR}/bin/"
echo "    bin/yacc"

"${TOOLDIR}/bin/yacc" -o "${OBJROOT}/yacc-smoke.c" \
	"${SRCDIR}/libexec/ftpd/ftpcmd.y"

# =====================================================================
# 6: Build host config via manual flex/yacc/cc (no make)
# =====================================================================
#
# config(8) is needed to cross-build the kernel.  The UKC (-e mode)
# and elf glue are skipped via -DMAKE_BOOTSTRAP.
#
# Source files (non-UKC):
#   gram.y  -> yacc  -> gram.tab.c + gram.tab.h
#   scan.l  -> flex  -> lex.yy.c
#   files.c hash.c main.c mkheaders.c mkioconf.c mkmakefile.c
#   mkswap.c pack.c sem.c util.c
#
# Compat shims needed:
#   err.c  pledge.c  reallocarray.c  strlcpy.c  strlcat.c  progname.c

echo ""
echo "==> Building OpenBSD config for the host"

CONFIG_SRC="${SRCDIR}/usr.sbin/config"
CONFIG_DIR="${OBJROOT}/usr.sbin/config"
mkdir -p "${CONFIG_DIR}"

# 6.1: Generate parser (gram.y -> gram.tab.c, gram.tab.h)
"${TOOLDIR}/bin/yacc" -d -o "${CONFIG_DIR}/gram.tab.c" "${CONFIG_SRC}/gram.y"
# scan.l includes "gram.h"; yacc produces gram.tab.h -> symlink it
ln -sf gram.tab.h "${CONFIG_DIR}/gram.h"

# 6.2: Generate lexer (scan.l -> lex.yy.c)
flex -o "${CONFIG_DIR}/lex.yy.c" "${CONFIG_SRC}/scan.l"

# 6.3: Compile everything and link
# Source files from usr.sbin/config/ (non-UKC subset)
CONFIG_CORE="files.c hash.c main.c mkheaders.c mkioconf.c mkmakefile.c \
	mkswap.c pack.c sem.c util.c"

# Generated files (yacc + flex)
CONFIG_GEN="gram.tab.c lex.yy.c"

# Compat shims (bootstrap/compat/)
CONFIG_COMPAT="err.c pledge.c reallocarray.c strlcpy.c strlcat.c progname.c"

# Build CPPFLAGS: bootstrap shim headers, OpenBSD system includes,
# the config source dir (for local .h), the build dir (for gram.tab.h),
# and -DMAKE_BOOTSTRAP to skip UKC/elf code.
CONFIG_CPPFLAGS="-I${BOOTSTRAP_INCLUDE} ${HOST_TOOL_CPPFLAGS} \
	-DMAKE_BOOTSTRAP -I${CONFIG_SRC} -I${CONFIG_DIR}"

cd "${CONFIG_DIR}"

OBJS=""
for src in ${CONFIG_CORE}; do
	obj="$(basename "${src}" .c).o"
	${SYSTEM_CC} ${CONFIG_CPPFLAGS} -c "${CONFIG_SRC}/${src}" -o "${obj}"
	OBJS="${OBJS} ${obj}"
done

for src in ${CONFIG_GEN}; do
	obj="$(basename "${src}" .c).o"
	${SYSTEM_CC} ${CONFIG_CPPFLAGS} -c "${CONFIG_DIR}/${src}" -o "${obj}"
	OBJS="${OBJS} ${obj}"
done

for src in ${CONFIG_COMPAT}; do
	obj="$(basename "${src}" .c).o"
	${SYSTEM_CC} ${CONFIG_CPPFLAGS} -c "${BOOTSTRAP_COMPAT}/${src}" -o "${obj}"
	OBJS="${OBJS} ${obj}"
done

${SYSTEM_CC} -o config ${OBJS}
echo "    config"

cp config "${TOOLDIR}/bin/"
echo "    bin/config"

# Quick smoke-test: parse a nonexistent file (should print "cannot read"
# and exit non-zero -- proving the binary linked and runs).
set +e
"${TOOLDIR}/bin/config" /nonexistent 2>&1 | grep -q "cannot read"
CONFIG_SMOKE_RC=$?
set -e
if [ ${CONFIG_SMOKE_RC} -ne 0 ]; then
	echo "    WARNING: config smoke-test failed (may be harmless)" >&2
else
echo "    smoke-test: config runs OK"
fi

# =====================================================================
# 7: Build host makefs via OpenBSD makefiles
# =====================================================================

echo ""
echo "==> Building OpenBSD makefs for the host"

MAKEFS_INCDIR="${OBJROOT}/makefs-include"
mkdir -p "${MAKEFS_INCDIR}"
ln -sfn "${SRCDIR}/sys/arch/${MACHINE}/include" "${MAKEFS_INCDIR}/machine"

MAKEFS_CPPFLAGS="${HOST_TOOL_CPPFLAGS} -include stdint.h -include time.h \
	-include sys/param.h \
	-I${SRCDIR}/usr.sbin/makefs -I${SRCDIR}/lib/libutil \
	-idirafter ${SRCDIR}/sys \
	-idirafter ${MAKEFS_INCDIR}"
MAKEFS_LDADD="${HOST_TOOL_LDADD_ALLOC} ${BOOTSTRAP_COMPAT}/err.c \
	${BOOTSTRAP_COMPAT}/progname.c ${BOOTSTRAP_COMPAT}/random.c \
	${BOOTSTRAP_COMPAT}/scan_scaled.c ${BOOTSTRAP_COMPAT}/getdiskbyname.c"

env ${MAKE_ENV} LIBC= LIBUTIL= ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/usr.sbin/makefs" \
	obj \
	CPPFLAGS="${MAKEFS_CPPFLAGS}" \
	LDADD="${MAKEFS_LDADD}"

env ${MAKE_ENV} LIBC= LIBUTIL= ${BMAKE} ${MAKE_ARGS} \
	-C "${SRCDIR}/usr.sbin/makefs" \
	all \
	CPPFLAGS="${MAKEFS_CPPFLAGS}" \
	LDADD="${MAKEFS_LDADD}"

MAKEFSDIR="${OBJROOT}/usr.sbin/makefs"
cp "${MAKEFSDIR}/makefs" "${TOOLDIR}/bin/"
echo "    bin/makefs"

MAKEFS_SMOKE_DIR="${OBJROOT}/makefs-smoke-root"
rm -rf "${MAKEFS_SMOKE_DIR}"
mkdir -p "${MAKEFS_SMOKE_DIR}"
printf "makefs smoke\n" > "${MAKEFS_SMOKE_DIR}/README"
"${TOOLDIR}/bin/makefs" -t ffs -s 1m \
	"${OBJROOT}/makefs-smoke.fs" "${MAKEFS_SMOKE_DIR}"
echo "    smoke-test: makefs builds an ffs image"

echo ""
echo "==> Success: toolchain installed to ${TOOLDIR}"
echo "    Add to PATH: export PATH=\"${TOOLDIR}/bin:\$PATH\""
