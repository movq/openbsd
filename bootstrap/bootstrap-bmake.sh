#!/bin/sh
#
# bootstrap-bmake.sh - bootstrap OpenBSD's make(1) on a supported host.
#
# Produces a "bmake" binary in the current directory.
#
# Usage: ./bootstrap/bootstrap-bmake.sh [OBJDIR=/path] [HOST_CC=cc]
#
set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "$SCRIPTDIR/.." && pwd)"

[ -n "${CC:-}" ] && HOST_CC="${HOST_CC:-${CC}}"
. "${SCRIPTDIR}/lib.sh"

CFLAGS="${CFLAGS:--O2 -g}"
MAKEDIR="$SRCDIR/usr.bin/make"
LSTDIR="$MAKEDIR/lst.lib"
UTILDIR="$SRCDIR/lib/libutil"
LIBCSTDLIBDIR="$SRCDIR/lib/libc/stdlib"

BUILDDIR="${BMAKE_OBJDIR:-${OBJROOT:-${HOST_OBJDIR}/bmake}}"
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"

# OpenBSD make intentionally hard-codes _PATH_BSHELL.  Override the host
# paths.h value so OpenBSD makefiles can use their expected ksh syntax.
cat > "${BUILDDIR}/paths.h" <<EOF
#include_next <paths.h>
#undef _PATH_BSHELL
#define _PATH_BSHELL "${HOST_SHELL}"
EOF

compile() {
	local src="$1"
	local obj="$2"
	shift 2
	echo "  CC      $(basename "$src")"
	set -x
	${HOST_CC} ${CFLAGS} "$@" -c -o "$obj" "$src"
	set +x
}

link() {
	local out="$1"
	shift
	echo "  LINK    $(basename "$out")"
	${HOST_CC} ${CFLAGS} "$@" -o "$out" ${LDFLAGS:-}
}

SHARED_INCLUDES="${HOST_FEATURE_CPPFLAGS}"

# Include paths needed by make sources
MAKE_INCLUDES="$SHARED_INCLUDES
	-I$MAKEDIR
	-I$LSTDIR
	-I$UTILDIR
	-I$BUILDDIR
	-DMAKE_BSIZE=256
	-DDEFMAXJOBS=4
"

# For the generate tool (host tool that produces hash headers)
GENERATE_INCLUDES="$SHARED_INCLUDES
	-I$MAKEDIR
	-I$UTILDIR
"

# The compat implementation files don't need all the make-local
# includes, just the shim headers.
COMPAT_INCLUDES="$SHARED_INCLUDES"

# Warning flags
WARNFLAGS="-std=gnu99 -Wno-attributes -Wno-unused-result -Wno-cpp"

CFLAGS="$CFLAGS $WARNFLAGS"

echo "==> Building bmake from $SRCDIR"
echo "    HOST_OS  = ${HOST_OS}"
echo "    HOST_CC  = ${HOST_CC}"
echo "    CFLAGS   = $CFLAGS"
echo "    BUILDDIR = $BUILDDIR"
echo ""


# 1. Compat
#
# Each host configuration supplies only the interfaces missing from its libc.
for src in ${BMAKE_COMPAT_SRCS}; do
	obj="$(basename "$src" .c).o"
	compile "$HOST_COMPAT/$src" "$BUILDDIR/$obj" \
		$COMPAT_INCLUDES -Wno-missing-prototypes
done

if [ "${BMAKE_USE_OPENBSD_GETOPT}" = yes ]; then
	# glibc getopt permutes arguments and ignores optreset.
	compile "$LIBCSTDLIBDIR/getopt_long.c" "$BUILDDIR/getopt_long.o" \
		$MAKE_INCLUDES
fi

# 2. ohash
compile "$UTILDIR/ohash.c" "$BUILDDIR/ohash.o" \
	$GENERATE_INCLUDES -Wno-missing-prototypes

# 3. 'generate' tool
# stats.c is needed by generate but HAS_STATS is not defined,
# so it compiles to essentially nothing (just includes headers).
compile "$MAKEDIR/stats.c" "$BUILDDIR/stats.o" \
	$GENERATE_INCLUDES -Wno-missing-prototypes

# memory.c provides emalloc/erealloc/hash callbacks used by ohash.
compile "$MAKEDIR/memory.c" "$BUILDDIR/memory.o" \
	$GENERATE_INCLUDES

# generate.c itself
compile "$MAKEDIR/generate.c" "$BUILDDIR/generate.o" \
	$GENERATE_INCLUDES

# Collect compat objects needed for the generate tool
GENERATE_COMPAT_OBJS=""
for src in ${BMAKE_COMPAT_SRCS}; do
	obj="$(basename "$src" .c).o"
	GENERATE_COMPAT_OBJS="$GENERATE_COMPAT_OBJS $BUILDDIR/$obj"
done

# Link the generate tool
link "$BUILDDIR/generate" \
	"$BUILDDIR/generate.o" \
	"$BUILDDIR/stats.o" \
	"$BUILDDIR/memory.o" \
	"$BUILDDIR/ohash.o" \
	$GENERATE_COMPAT_OBJS

# Run generate to produce hash headers
echo "  GEN     varhashconsts.h"
"$BUILDDIR/generate" 1 36 > "$BUILDDIR/varhashconsts.h"
echo "  GEN     condhashconsts.h"
"$BUILDDIR/generate" 2 65 > "$BUILDDIR/condhashconsts.h"
echo "  GEN     nodehashconsts.h"
"$BUILDDIR/generate" 3 0  > "$BUILDDIR/nodehashconsts.h"

# 4. Make sources
# List of all .c files that make needs (from usr.bin/make/Makefile SRCS)
# plus the lst.lib files.
MAKE_SRCS="
	arch.c buf.c cmd_exec.c compat.c cond.c dir.c direxpand.c dump.c
	engine.c enginechoice.c error.c expandchildren.c
	for.c init.c job.c lowparse.c main.c make.c memory.c parse.c
	parsevar.c str.c stats.c suff.c targ.c targequiv.c timestamp.c
	var.c varmodifiers.c varname.c
"

LST_SRCS="
	lstAddNew.c lstAppend.c lstConcat.c lstConcatDestroy.c
	lstDeQueue.c lstDestroy.c lstDupl.c lstFindFrom.c lstForEachFrom.c
	lstInsert.c lstMember.c lstRemove.c lstSucc.c
"

# Files that depend on generated headers
GENERATED_DEPS="var.c cond.c parse.c targ.c"

# Build non-generated-dep files first (parallel-safe order not needed here)
for src in $MAKE_SRCS; do
	# Skip sources we have special handling for
	case "$src" in
		generate.c|regress.c) continue ;;
	esac

	# First pass: compile files that don't need generated headers
	in_list=false
	for dep in $GENERATED_DEPS; do
		[ "$src" = "$dep" ] && in_list=true && break
	done
	if ! $in_list; then
		obj="$(basename "$src" .c).o"
		compile "$MAKEDIR/$src" "$BUILDDIR/$obj" $MAKE_INCLUDES
	fi
done

# Build lst.lib files
for src in $LST_SRCS; do
	obj="$(basename "$src" .c).o"
	compile "$LSTDIR/$src" "$BUILDDIR/$obj" $MAKE_INCLUDES
done

# Now the files that need generated headers (hashconsts.h)
for src in $GENERATED_DEPS; do
	obj="$(basename "$src" .c).o"
	compile "$MAKEDIR/$src" "$BUILDDIR/$obj" $MAKE_INCLUDES
done

# 5. Link make
# Collect all make .o files we produced
OBJS=""
for src in $MAKE_SRCS; do
	case "$src" in
		generate.c|regress.c) continue ;;
	esac
	obj="$(basename "$src" .c).o"
	OBJS="$OBJS $BUILDDIR/$obj"
done
for src in $LST_SRCS; do
	obj="$(basename "$src" .c).o"
	OBJS="$OBJS $BUILDDIR/$obj"
done

# Add ohash, the host compatibility layer, and the Linux getopt replacement.
OBJS="$OBJS $BUILDDIR/ohash.o $GENERATE_COMPAT_OBJS"
if [ "${BMAKE_USE_OPENBSD_GETOPT}" = yes ]; then
	OBJS="$OBJS $BUILDDIR/getopt_long.o"
fi

link "$BUILDDIR/bmake" $OBJS ${BMAKE_LDLIBS}

mkdir -p "$SCRIPTDIR/tools/bin"
cp "$BUILDDIR/bmake" "$SCRIPTDIR/tools/bin/bmake"
echo ""
echo "==> Success: $SCRIPTDIR/tools/bin/bmake"
