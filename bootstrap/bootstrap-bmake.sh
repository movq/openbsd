#!/bin/sh
#
# build.sh - bootstrap OpenBSD's make(1) on a GNU/Linux host.
#
# Produces a "bmake" binary in the current directory.
#
# Uses an include shim tree (bootstrap/include/) to provide BSD
# declarations missing on glibc, without patching OpenBSD sources
# or force-including a compat header.
#
# Usage: ./build.sh [CC=cc]
#
set -e

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2 -g}"

# Resolve paths relative to this script's location
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "$SCRIPTDIR/.." && pwd)"

MAKEDIR="$SRCDIR/usr.bin/make"
LSTDIR="$MAKEDIR/lst.lib"
UTILDIR="$SRCDIR/lib/libutil"
LIBCSTDLIBDIR="$SRCDIR/lib/libc/stdlib"

BOOTSTRAP_INCLUDE="$SCRIPTDIR/include"
BOOTSTRAP_COMPAT="$SCRIPTDIR/compat"

# Build output directory
BUILDDIR="$SCRIPTDIR/build"
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"

compile() {
	local src="$1"
	local obj="$2"
	shift 2
	echo "  CC      $(basename "$src")"
	$CC $CFLAGS "$@" -c -o "$obj" "$src"
}

link() {
	local out="$1"
	shift
	echo "  LINK    $(basename "$out")"
	$CC $CFLAGS "$@" -o "$out" $LDFLAGS
}

# _DEFAULT_SOURCE gives us setenv, strdup, strnlen, etc. on glibc
# without exposing deprecated XSI interfaces like sigset().
# The -I bootstrap/include path provides shim headers (using
# #include_next) that add BSD declarations missing on glibc.
SHARED_INCLUDES="-I$BOOTSTRAP_INCLUDE -D_DEFAULT_SOURCE"

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
WARNFLAGS="-std=gnu99 -Wno-attributes -Wno-unused-result"

CFLAGS="$CFLAGS $WARNFLAGS"

echo "==> Building bmake from $SRCDIR"
echo "    CC       = $CC"
echo "    CFLAGS   = $CFLAGS"
echo "    BUILDDIR = $BUILDDIR"
echo ""


# 1. Compat
#
# Each compat .c file provides one BSD function missing on glibc.
# They include standard headers (e.g. <err.h>, <stdlib.h>) which
# resolve via the shim tree at bootstrap/include/.
COMPAT_SRCS="err.c reallocarray.c strtonum.c fgetln.c pledge.c sys_signame.c"
for src in $COMPAT_SRCS; do
	obj="$(basename "$src" .c).o"
	compile "$BOOTSTRAP_COMPAT/$src" "$BUILDDIR/$obj" \
		$COMPAT_INCLUDES -Wno-missing-prototypes
done

# Compile OpenBSD's getopt to replace glibc's getopt (which permutes
# arguments and ignores optreset, breaking MainParseArgs).
compile "$LIBCSTDLIBDIR/getopt_long.c" "$BUILDDIR/getopt_long.o" \
	$MAKE_INCLUDES

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
for src in $COMPAT_SRCS; do
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

# Add ohash, compat layer, and getopt
OBJS="$OBJS $BUILDDIR/ohash.o $GENERATE_COMPAT_OBJS $BUILDDIR/getopt_long.o"

link "$BUILDDIR/bmake" $OBJS -lrt

mkdir -p "$SCRIPTDIR/tools/bin"
cp "$BUILDDIR/bmake" "$SCRIPTDIR/tools/bin/bmake"
echo ""
echo "==> Success: $SCRIPTDIR/tools/bin/bmake"
