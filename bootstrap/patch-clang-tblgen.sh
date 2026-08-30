#!/bin/sh
#
# patch-clang-tblgen.sh - Replace hardcoded tblgen binary paths in
# gnu/usr.bin/clang/ Makefiles with variables that resolve to host-native
# tools at build time.
#
# OpenBSD's clang Makefiles reference tblgen binaries via ${.OBJDIR}/.../,
# but those are OpenBSD executables that cannot run on the build host.
# This script replaces those references with ${LLVM_TBLGEN},
# ${LLVM_MIN_TBLGEN}, ${CLANG_TBLGEN}, and ${LLDB_TBLGEN} which are
# defined in bootstrap's mk.conf and point to host-native tools built
# separately.
#
# Usage:
#   ./bootstrap/patch-clang-tblgen.sh [path/to/gnu/usr.bin/clang]
#

set -e

CLANGDIR="${1:-$(dirname "$0")/../gnu/usr.bin/clang}"

if [ ! -d "$CLANGDIR" ]; then
	echo "error: clang directory not found: $CLANGDIR" >&2
	exit 1
fi

echo "Patching tblgen paths in ${CLANGDIR}..."

find "$CLANGDIR" -name Makefile -print | while IFS= read -r makefile; do
	tmp="${makefile}.bootstrap.$$"
	sed \
	    -e 's|\${\.OBJDIR}/\(\.\./\)\{1,\}llvm-tblgen/llvm-tblgen|\${LLVM_TBLGEN}|g' \
	    -e 's|\${\.OBJDIR}/\(\.\./\)\{1,\}llvm-min-tblgen/llvm-min-tblgen|\${LLVM_MIN_TBLGEN}|g' \
	    -e 's|\${\.OBJDIR}/\(\.\./\)\{1,\}clang-tblgen/clang-tblgen|\${CLANG_TBLGEN}|g' \
	    -e 's|\${\.OBJDIR}/\(\.\./\)\{1,\}lldb-tblgen/lldb-tblgen|\${LLDB_TBLGEN}|g' \
	    "${makefile}" > "${tmp}"
	if cmp -s "${makefile}" "${tmp}"; then
		rm -f "${tmp}"
	else
		mv "${tmp}" "${makefile}"
	fi
done

echo "Done patching tblgen paths."
