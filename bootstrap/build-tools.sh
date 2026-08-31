#!/bin/sh
#
# build-tools.sh - build OpenBSD tools that run on the host.
#
# Usage: ./bootstrap/build-tools.sh [OBJDIR=/path] [MACHINE_ARCH=amd64]
#
set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "$SCRIPTDIR/.." && pwd)"

MACHINE="${MACHINE:-${MACHINE_ARCH:-amd64}}"
MACHINE_ARCH="${MACHINE_ARCH:-amd64}"
[ -n "${CC:-}" ] && HOST_CC="${HOST_CC:-${CC}}"
[ -n "${CXX:-}" ] && HOST_CXX="${HOST_CXX:-${CXX}}"
. "${SCRIPTDIR}/lib.sh"

BMAKE="${BMAKE:-${SCRIPTDIR}/tools/bin/bmake}"
if [ ! -x "${BMAKE}" ] && [ -x "${HOST_OBJDIR}/bmake/bmake" ]; then
	BMAKE="${HOST_OBJDIR}/bmake/bmake"
fi
OBJROOT="${OBJROOT:-${HOST_OBJDIR}}"
OBJROOT="$(mkdir -p "${OBJROOT}" && cd "${OBJROOT}" && pwd)"

WRAP_CC="${WRAPDIR}/cc"
WRAP_CXX="${WRAPDIR}/c++"
LEX="${LEX:-flex}"

bootstrap_need_exec "${BMAKE}"
bootstrap_need_tool "${HOST_CC}"
bootstrap_need_tool "${HOST_CXX}"
bootstrap_need_tool "${LEX}"
bootstrap_need_tool perl

# Variables that must reach bmake (and its sub-makes) are exported.
export MACHINE_ARCH
export MACHINE
export REAL_CC="${HOST_CC}"
export REAL_CXX="${HOST_CXX}"

# Prepend wrap directory to PATH so that our lorder/tsort wrappers
# are found before system tools.  bmake runs shell commands using
# the invoking environment's PATH.
export PATH="${WRAPDIR}:${PATH}"

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
#   BUILD_LLDB=no      - lldb is not needed for the cross-build
#   PATH               - prepended with wrap/ for lorder/tsort wrappers
#   LD=ld              - use system linker for partial (relocatable) links
#   AR=ar              - use system ar
#   RANLIB=ranlib      - use system ranlib
#   AR_VERSION=binutils - skip llvm-ar build
#   LIBCRT0/CRTBEGIN/CRTEND - target startup objects are not host inputs
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
	BUILDUSER=${HOST_USER}
	BINOWN=${HOST_USER}
	BINGRP=${HOST_GROUP}
	LIBOWN=${HOST_USER}
	LIBGRP=${HOST_GROUP}
	MAKE=${BMAKE}
	CC=${WRAP_CC}
	CXX=${WRAP_CXX}
"

HOST_TOOL_LDADD="$(bootstrap_source_list "${HOST_COMPAT}" ${HOST_TOOL_COMPAT_SRCS})"
HOST_TOOL_LDADD_ALLOC="$(bootstrap_source_list "${HOST_COMPAT}" ${HOST_TOOL_ALLOC_COMPAT_SRCS})"

echo "==> Building OpenBSD LLVM/clang toolchain"
echo "    SRCDIR       = ${SRCDIR}"
echo "    BMAKE        = ${BMAKE}"
echo "    HOST_OS      = ${HOST_OS}"
echo "    MACHINE_ARCH = ${MACHINE_ARCH}"
echo "    HOST_CC      = ${HOST_CC}"
echo "    HOST_CXX     = ${HOST_CXX}"
echo "    HOST_SHELL   = ${HOST_SHELL}"
echo "    TOOLDIR      = ${TOOLDIR}"
echo "    OBJROOT      = ${OBJROOT}"
echo "    JOBS         = ${JOBS}"
echo ""

# These tools are built specifically for OpenBSD targets.  Select OpenBSD
# behavior without relying on the host preprocessor, then restore the sources.
LLD_ELF_DIR="${SRCDIR}/gnu/llvm/lld/ELF"
LLD_ELF_BACKUP="${OBJROOT}/lld-elf-openbsd.orig"
COMMON_ARGS="${SRCDIR}/gnu/llvm/clang/lib/Driver/ToolChains/CommonArgs.cpp"
COMMON_ARGS_TMP="${OBJROOT}/CommonArgs.cpp.tmp"
COMMON_ARGS_BACKUP="${OBJROOT}/CommonArgs.cpp.orig"

restore_llvm_sources()
{
	if [ -f "${COMMON_ARGS_BACKUP}" ]; then
		cp -p "${COMMON_ARGS_BACKUP}" "${COMMON_ARGS}"
		rm -f "${COMMON_ARGS_BACKUP}"
	fi
	if [ -d "${LLD_ELF_BACKUP}" ]; then
		find "${LLD_ELF_BACKUP}" -type f | while IFS= read -r backup; do
			relative="${backup#${LLD_ELF_BACKUP}/}"
			cp -p "${backup}" "${LLD_ELF_DIR}/${relative}"
		done
		rm -rf "${LLD_ELF_BACKUP}"
	fi
}

# Recover first if a previous build was killed before its trap completed.
restore_llvm_sources
trap restore_llvm_sources EXIT HUP INT TERM

echo "==> Selecting OpenBSD branches in lld ELF backend"
find "${LLD_ELF_DIR}" -type f \( -name '*.cpp' -o -name '*.h' \) |
while IFS= read -r src; do
	grep -q '__OpenBSD__' "${src}" || continue
	relative="${src#${LLD_ELF_DIR}/}"
	mkdir -p "${LLD_ELF_BACKUP}/$(dirname "${relative}")"
	cp -p "${src}" "${LLD_ELF_BACKUP}/${relative}"
	sed -e 's/#ifdef __OpenBSD__/#if 1/' \
		-e 's/#ifndef __OpenBSD__/#if 0/' \
		-e 's/defined(__OpenBSD__)/1/g' \
		"${src}" > "${src}.tmp"
	mv "${src}.tmp" "${src}"
done
echo ""

cp -p "${COMMON_ARGS}" "${COMMON_ARGS_BACKUP}"
sed -e 's/#ifdef __OpenBSD__/#if 1/' \
	-e 's/#ifndef __OpenBSD__/#if 0/' \
	-e 's/defined(__OpenBSD__)/1/g' \
	"${COMMON_ARGS}" > "${COMMON_ARGS_TMP}"
if ! cmp -s "${COMMON_ARGS}" "${COMMON_ARGS_TMP}"; then
	mv "${COMMON_ARGS_TMP}" "${COMMON_ARGS}"
else
	rm -f "${COMMON_ARGS_TMP}"
fi

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
env ${MAKE_ENV} CPPFLAGS="${LLVM_HOST_CPPFLAGS}" \
	LDADD="${LLVM_HOST_LDADD}" \
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
"${LEX}" -o "${CONFIG_DIR}/lex.yy.c" "${CONFIG_SRC}/scan.l"

# 6.3: Compile everything and link
# Source files from usr.sbin/config/ (non-UKC subset)
CONFIG_CORE="files.c hash.c main.c mkheaders.c mkioconf.c mkmakefile.c \
	mkswap.c pack.c sem.c util.c"

# Generated files (yacc + flex)
CONFIG_GEN="gram.tab.c lex.yy.c"

# Build CPPFLAGS: bootstrap shim headers, OpenBSD system includes,
# the config source dir (for local .h), the build dir (for gram.tab.h),
# and -DMAKE_BOOTSTRAP to skip UKC/elf code.
CONFIG_CPPFLAGS="${HOST_TOOL_CPPFLAGS} \
	-DMAKE_BOOTSTRAP -I${CONFIG_SRC} -I${CONFIG_DIR}"

cd "${CONFIG_DIR}"

OBJS=""
for src in ${CONFIG_CORE}; do
	obj="$(basename "${src}" .c).o"
	${HOST_CC} ${CONFIG_CPPFLAGS} -c "${CONFIG_SRC}/${src}" -o "${obj}"
	OBJS="${OBJS} ${obj}"
done

for src in ${CONFIG_GEN}; do
	obj="$(basename "${src}" .c).o"
	${HOST_CC} ${CONFIG_CPPFLAGS} -c "${CONFIG_DIR}/${src}" -o "${obj}"
	OBJS="${OBJS} ${obj}"
done

for src in ${CONFIG_COMPAT_SRCS}; do
	obj="$(basename "${src}" .c).o"
	${HOST_CC} ${CONFIG_CPPFLAGS} -c "${HOST_COMPAT}/${src}" -o "${obj}"
	OBJS="${OBJS} ${obj}"
done

${HOST_CC} -o config ${OBJS}
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
MAKEFS_LDADD="$(bootstrap_source_list "${HOST_COMPAT}" ${MAKEFS_COMPAT_SRCS})"

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

restore_llvm_sources
trap - EXIT HUP INT TERM
