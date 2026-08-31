#!/bin/sh
#
# Shared setup for building OpenBSD on a non-OpenBSD host.

bootstrap_die()
{
	echo "bootstrap: $*" >&2
	exit 1
}

bootstrap_need_tool()
{
	if ! command -v "$1" >/dev/null 2>&1; then
		bootstrap_die "missing required host tool: $1"
	fi
}

bootstrap_need_file()
{
	if [ ! -f "$1" ]; then
		bootstrap_die "missing required file: $1"
	fi
}

bootstrap_need_exec()
{
	if [ ! -x "$1" ]; then
		bootstrap_die "missing required executable: $1"
	fi
}

bootstrap_real_dir()
{
	mkdir -p "$1"
	(cd "$1" && pwd)
}

bootstrap_resolve_tool()
{
	tool_path="$(command -v "$1" 2>/dev/null)" ||
	    bootstrap_die "missing required host tool: $1"
	case "${tool_path}" in
	/*)
		echo "${tool_path}"
		;;
	*)
		tool_dir="$(dirname "${tool_path}")"
		tool_name="$(basename "${tool_path}")"
		echo "$(cd "${tool_dir}" && pwd)/${tool_name}"
		;;
	esac
}

bootstrap_jobs()
{
	if command -v nproc >/dev/null 2>&1; then
		nproc
	elif getconf NPROCESSORS_ONLN >/dev/null 2>&1; then
		getconf NPROCESSORS_ONLN
	elif sysctl -n hw.ncpuonline >/dev/null 2>&1; then
		sysctl -n hw.ncpuonline
	elif sysctl -n hw.ncpu >/dev/null 2>&1; then
		sysctl -n hw.ncpu
	else
		echo 1
	fi
}

bootstrap_source_list()
{
	dir="$1"
	shift

	result=
	for src do
		result="${result}${result:+ }${dir}/${src}"
	done
	echo "${result}"
}

bootstrap_write_cross_cc()
{
	output="$1"
	compiler="$2"
	target="$3"
	sysroot="$4"
	linker="$5"
	driver_mode="${6:-}"

	if [ -n "${driver_mode}" ]; then
		driver_arg="--driver-mode=${driver_mode}"
	else
		driver_arg=
	fi
	if [ -n "${sysroot}" ]; then
		sysroot_arg="--sysroot=${sysroot}"
	else
		sysroot_arg=
	fi

	cat > "${output}" <<EOF
#!/bin/sh
compile_only=no
for arg do
	case "\${arg}" in
	-c|-S|-E|-M|-MM) compile_only=yes ;;
	esac
done
if [ "\${compile_only}" = yes ]; then
	exec "${compiler}" ${driver_arg} --target="${target}" ${sysroot_arg} "\$@"
fi
exec "${compiler}" ${driver_arg} --target="${target}" ${sysroot_arg} \
    -fuse-ld="${linker}" "\$@"
EOF
	chmod +x "${output}"
}

bootstrap_write_install()
{
	output="$1"
	host_install="$2"

	cat > "${output}" <<EOF
#!/bin/sh
# Adapt OpenBSD install flags for an unprivileged staging directory.
exec perl -e '
	my \$install = shift;
	my @filtered;
	my \$create_dest = 0;
	while (@ARGV) {
		my \$arg = shift;
		if (\$arg eq "-D") {
			\$create_dest = 1;
			next;
		}
		if (\$arg eq "-o" || \$arg eq "-g" ||
		    \$arg eq "--owner" || \$arg eq "--group") {
			shift if @ARGV;
			next;
		}
		next if \$arg =~ /^(?:-[og].+|--(?:owner|group)=)/;
		push @filtered, \$arg;
	}
	if (\$create_dest) {
		require File::Basename;
		require File::Path;
		my \$dir = File::Basename::dirname(\$filtered[-1]);
		File::Path::make_path(\$dir);
	}
	exec {\$install} \$install, @filtered;
	die "exec \$install: \$!\\n";
' "${host_install}" "\$@"
EOF
	chmod +x "${output}"
}

if [ -z "${SCRIPTDIR:-}" ] || [ -z "${SRCDIR:-}" ]; then
	bootstrap_die "SCRIPTDIR and SRCDIR must be set before sourcing lib.sh"
fi

HOST_OS="${HOST_OS:-$(uname -s)}"
case "${HOST_OS}" in
Linux)
	HOST_PLATFORM=linux
	;;
NetBSD)
	HOST_PLATFORM=netbsd
	;;
FreeBSD)
	HOST_PLATFORM=freebsd
	;;
*)
	bootstrap_die "unsupported host operating system: ${HOST_OS}"
	;;
esac

HOST_PLATFORM_DIR="${SCRIPTDIR}/platform/${HOST_PLATFORM}"
bootstrap_need_file "${HOST_PLATFORM_DIR}/config.sh"
. "${HOST_PLATFORM_DIR}/config.sh"

OBJDIR="${OBJDIR:-${SCRIPTDIR}/obj}"
OBJDIR="$(bootstrap_real_dir "${OBJDIR}")"
HOST_OBJDIR="${HOST_OBJDIR:-${OBJDIR}/host}"
TARGET_OBJDIR="${TARGET_OBJDIR:-${OBJDIR}/target-${MACHINE:-amd64}}"

JOBS="${JOBS:-$(bootstrap_jobs)}"
case "${JOBS}" in
''|*[!0-9]*|0)
	bootstrap_die "JOBS must be a positive integer"
	;;
esac

HOST_CC="${HOST_CC:-${HOSTCC:-${HOST_CC_DEFAULT}}}"
HOST_CXX="${HOST_CXX:-${HOSTCXX:-${HOST_CXX_DEFAULT}}}"
HOST_SHELL="${HOST_SHELL:-${HOST_SHELL_DEFAULT}}"
HOST_USER="${HOST_USER:-${BUILDUSER:-$(id -un)}}"
HOST_GROUP="${HOST_GROUP:-${BUILDGROUP:-$(id -gn)}}"

HOST_CC="$(bootstrap_resolve_tool "${HOST_CC}")"
HOST_CXX="$(bootstrap_resolve_tool "${HOST_CXX}")"
bootstrap_need_exec "${HOST_SHELL}"

BMAKE="${BMAKE:-${SCRIPTDIR}/tools/bin/bmake}"
TOOLDIR="${TOOLDIR:-${SCRIPTDIR}/tools}"
WRAPDIR="${SCRIPTDIR}/wrap"
MAKE_ARGS="-m ${SRCDIR}/share/mk -j ${JOBS}"
