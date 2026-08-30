# Host configuration for GNU/Linux.

HOST_CC_DEFAULT=clang
HOST_CXX_DEFAULT=clang++
HOST_SHELL_DEFAULT=/bin/bash

HOST_INCLUDE="${HOST_PLATFORM_DIR}/include"
HOST_COMPAT="${HOST_PLATFORM_DIR}/compat"

HOST_FEATURE_CPPFLAGS="-I${HOST_INCLUDE} -I/usr/include/bsd -D_DEFAULT_SOURCE -DLIBBSD_OVERLAY"
HOST_TOOL_CPPFLAGS="${HOST_FEATURE_CPPFLAGS} -idirafter ${SRCDIR}/include"
LLVM_HOST_CPPFLAGS="-I/usr/include/bsd -DLIBBSD_OVERLAY"
LLVM_HOST_LDADD=
BMAKE_LDLIBS="-lrt -lbsd"
BMAKE_USE_OPENBSD_GETOPT=yes
BMAKE_COMPAT_SRCS="err.c reallocarray.c strtonum.c fgetln.c pledge.c sys_signame.c"

HOST_TOOL_COMPAT_SRCS="pledge.c unveil.c strlcpy.c strlcat.c"
HOST_TOOL_ALLOC_COMPAT_SRCS="${HOST_TOOL_COMPAT_SRCS} reallocarray.c"
CONFIG_COMPAT_SRCS="err.c pledge.c reallocarray.c strlcpy.c strlcat.c progname.c"
MAKEFS_COMPAT_SRCS="${HOST_TOOL_ALLOC_COMPAT_SRCS} err.c progname.c random.c scan_scaled.c getdiskbyname.c"
MAKE_GENERATOR_COMPAT_SRCS="${SRCDIR}/lib/libutil/ohash.c ${HOST_COMPAT}/strtonum.c"
