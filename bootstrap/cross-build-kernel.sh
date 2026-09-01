#!/bin/sh
#
# cross-build-kernel.sh - cross-build an OpenBSD kernel.
#
# Uses bootstrap/tools/bin/config (already built by build-tools.sh) to
# generate a kernel build directory from a config file, then invokes
# bmake with the cross-compiler toolchain.
#
# Usage: OBJDIR=/path KERNEL_CONFIG=GENERIC.MP \
#   ./bootstrap/cross-build-kernel.sh
#
set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "${SCRIPTDIR}/.." && pwd)"

MACHINE="${MACHINE:-amd64}"
MACHINE_ARCH="${MACHINE_ARCH:-amd64}"
MACHINE_CPU="${MACHINE_CPU:-${MACHINE_ARCH}}"
. "${SCRIPTDIR}/lib.sh"

OBJROOT="${OBJROOT:-${TARGET_OBJDIR}}"
OBJROOT="$(bootstrap_real_dir "${OBJROOT}")"
CONFIG_BIN="${CONFIG_BIN:-${TOOLDIR}/bin/config}"

TARGET_CANON="${TARGET_CANON:-${MACHINE_ARCH}-unknown-openbsd7.9}"
KERNEL_CONFIG="${KERNEL_CONFIG:-GENERIC.MP}"

find_gnu_binutil() {
	requested="$1"
	name="$2"

	for candidate in "${requested}" \
	    "$(command -v "g${name}" 2>/dev/null || true)" \
	    "/usr/local/bin/${name}" \
	    "$(command -v "${name}" 2>/dev/null || true)" \
	    "/usr/bin/${name}"; do
		[ -x "${candidate}" ] || continue
		case "$("${candidate}" --version 2>/dev/null | sed -n '1p')" in
		"GNU ${name}"*)
			echo "${candidate}"
			return
			;;
		esac
	done
	bootstrap_die "missing required GNU-compatible host tool: ${name}"
}

CLANG="${CLANG:-${TOOLDIR}/bin/clang}"
LLD="${LLD:-${TOOLDIR}/bin/ld.lld}"
SIZE="${SIZE:-size}"
HOST_OBJCOPY="$(find_gnu_binutil "${HOST_OBJCOPY:-}" objcopy)"
HOST_OBJDUMP="$(find_gnu_binutil "${HOST_OBJDUMP:-}" objdump)"
HOST_PERL="$(bootstrap_resolve_tool "${HOST_PERL:-perl}")"

TARGET_TOOLDIR="${TARGET_TOOLDIR:-${OBJDIR}/target-tools-${MACHINE}}"
KERNEL_BINDIR="${TARGET_TOOLDIR}/bin"
MAKEGAP_SRC="${SCRIPTDIR}/makegap-host.sh"

install_if_changed() {
	src="$1"
	dst="$2"

	if [ -f "${dst}" ] && cmp -s "${src}" "${dst}"; then
		rm -f "${src}"
		return 1
	fi

	mv -f "${src}" "${dst}"
	return 0
}

# ------------------------------------------------------------------
# Pre-flight checks
# ------------------------------------------------------------------
bootstrap_need_exec "${BMAKE}"
bootstrap_need_exec "${CLANG}"
bootstrap_need_exec "${LLD}"
bootstrap_need_exec "${CONFIG_BIN}"
bootstrap_need_file "${MAKEGAP_SRC}"
bootstrap_need_tool sort
bootstrap_need_tool mktemp
bootstrap_need_tool "${SIZE}"
if [ -z "${OBJCOPY:-}" ]; then
	[ -n "${HOST_OBJCOPY}" ] ||
	    bootstrap_die "missing required host tool: objcopy"
	bootstrap_need_exec "${HOST_OBJCOPY}"
fi
bootstrap_need_exec "${HOST_OBJDUMP}"
bootstrap_need_exec "${HOST_PERL}"

# ------------------------------------------------------------------
# Create tool wrappers
# ------------------------------------------------------------------
mkdir -p "${KERNEL_BINDIR}"

# Cross-compiler wrapper: kernel compilation uses -nostdinc and
# -ffreestanding already, so --sysroot is unnecessary.  Just set
# the target triple so clang emits amd64 machine code.
bootstrap_write_cross_cc "${KERNEL_BINDIR}/openbsd-kernel-cc" \
	"${CLANG}" "${TARGET_CANON}" "" "${LLD}"

# OpenBSD kernels use ctfstrip(1) to convert debug info into CTF.
# A compatible host ctfstrip is not assumed.  Replace it with a no-op so the
# unstripped bsd.gdb is kept and bsd is a copy of it.
cat > "${KERNEL_BINDIR}/openbsd-strip-noop" <<'EOF'
#!/bin/sh
# No-op strip: just copy input to output if arguments suggest a
# strip invocation.
#
# ctfstrip is called as:  ctfstrip -S -o bsd bsd.gdb
# We replicate the mv + copy that the Makefile does around it.
src=""
dst=""
while [ $# -gt 0 ]; do
	case "$1" in
		-o) dst="$2"; shift 2 ;;
		-S) shift ;;
		-*) shift ;;
		*)  src="$1"; shift ;;
	esac
done
if [ -n "$src" ] && [ -n "$dst" ]; then
	cp "$src" "$dst"
fi
EOF
chmod +x "${KERNEL_BINDIR}/openbsd-strip-noop"

if [ -z "${OBJCOPY:-}" ]; then
	cat > "${KERNEL_BINDIR}/openbsd-efi-objcopy" <<EOF
#!/bin/sh

real_objcopy="${HOST_OBJCOPY}"
efi_format=
for arg do
	case "\${arg}" in
	--target=efi-app-x86_64|--target=efi-app-ia32)
		efi_format="\${arg#--target=}"
		;;
	efi-app-x86_64|efi-app-ia32)
		efi_format="\${arg}"
		;;
	esac
done

if [ -n "\${efi_format}" ]; then
	while [ "\$#" -gt 2 ]; do
		shift
	done
	exec "\${real_objcopy}" \
	    -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel \
	    -j .rel.dyn -j .rela -j .rela.dyn -j .reloc \
	    -O "\${efi_format}" "\$1" "\$2"
fi

exec "\${real_objcopy}" "\$@"
EOF
	chmod +x "${KERNEL_BINDIR}/openbsd-efi-objcopy"
fi

# check-boot.pl hardcodes /usr/bin/objdump and parses GNU objdump output.
# Route only that script through the host objdump selected before PATH changes.
cat > "${KERNEL_BINDIR}/perl" <<EOF
#!/bin/sh
case "\${1##*/}" in
check-boot.pl)
	checker="\$1"
	shift
	tmp="\$(mktemp "\${TMPDIR:-/tmp}/check-boot.pl.XXXXXXXXXX")" ||
	    exit 1
	trap 'rm -f "\${tmp}"' EXIT HUP INT TERM
	sed 's|/usr/bin/objdump|${HOST_OBJDUMP}|g' "\${checker}" > "\${tmp}"
	"${HOST_PERL}" -w "\${tmp}" "\$@"
	status=\$?
	rm -f "\${tmp}"
	trap - EXIT HUP INT TERM
	exit "\${status}"
	;;
esac
exec "${HOST_PERL}" "\$@"
EOF
chmod +x "${KERNEL_BINDIR}/perl"

# Prepend kernel wrapper directory to PATH.
export PATH="${KERNEL_BINDIR}:${TOOLDIR}/bin:${WRAPDIR}:${PATH}"
export MACHINE MACHINE_ARCH MACHINE_CPU

# Kernel build directory determined early so we can set up MAKECONF.
CONFIG_SRC="${SRCDIR}/sys/arch/${MACHINE}/conf/${KERNEL_CONFIG}"
KERNEL_BUILDDIR="${OBJROOT}/sys/arch/${MACHINE}/compile/${KERNEL_CONFIG}"
KERNEL_MKCONF="${KERNEL_BUILDDIR}/Makefile.mk.conf"

# ------------------------------------------------------------------
# All make variable overrides passed as environment variables.
# Most mirror what cross-build-userland.sh uses, with a few
# kernel-specific additions (STRIP, SIZE).
MAKE_ENV="
	BSDSRCDIR=${SRCDIR}
	BSDOBJDIR=${OBJROOT}
	MACHINE=${MACHINE}
	MACHINE_ARCH=${MACHINE_ARCH}
	MACHINE_CPU=${MACHINE_CPU}
	COMPILER_VERSION=clang
	LINKER_VERSION=lld
	WARNINGS=no
	NOMAN=1
	NOPROFILE=1
	MAKECONF=${KERNEL_MKCONF}
	MAKE=${BMAKE}
	CC=${KERNEL_BINDIR}/openbsd-kernel-cc
	LD=${LLD}
	STRIP=${KERNEL_BINDIR}/openbsd-strip-noop
	SIZE=${SIZE}
	HOSTCC=${HOST_CC}
	BUILDUSER=${HOST_USER}
	BINOWN=${HOST_USER}
	BINGRP=${HOST_GROUP}
	LIBOWN=${HOST_USER}
	LIBGRP=${HOST_GROUP}
	SHAREOWN=${HOST_USER}
	SHAREGRP=${HOST_GROUP}
	MANOWN=${HOST_USER}
	MANGRP=${HOST_GROUP}
"

# ------------------------------------------------------------------
# Run config to generate the kernel build directory
# ------------------------------------------------------------------
bootstrap_need_file "${CONFIG_SRC}"

echo "==> Cross-building OpenBSD kernel"
echo "    SRCDIR        = ${SRCDIR}"
echo "    BMAKE         = ${BMAKE}"
echo "    HOST_OS       = ${HOST_OS}"
echo "    CONFIG        = ${CONFIG_BIN}"
echo "    TARGET        = ${TARGET_CANON}"
echo "    MACHINE       = ${MACHINE}"
echo "    MACHINE_ARCH  = ${MACHINE_ARCH}"
echo "    KERNEL_CONFIG = ${KERNEL_CONFIG}"
echo "    BUILDDIR      = ${KERNEL_BUILDDIR}"
echo "    CC            = ${KERNEL_BINDIR}/openbsd-kernel-cc"
echo "    LD            = ${LLD}"
echo "    JOBS          = ${JOBS}"
echo ""

if [ "${KERNEL_RECONFIG:-no}" = yes ] || [ ! -f "${KERNEL_BUILDDIR}/Makefile" ]; then
	echo "==> Generating kernel build directory with config(8)"
	if [ "${KERNEL_RECONFIG:-no}" = yes ]; then
		rm -rf "${KERNEL_BUILDDIR}"
	fi
	mkdir -p "$(dirname "${KERNEL_BUILDDIR}")"
	"${CONFIG_BIN}" -b "${KERNEL_BUILDDIR}" -s "${SRCDIR}/sys" "${CONFIG_SRC}"
	echo "    Build directory generated: ${KERNEL_BUILDDIR}"
else
	echo "==> Reusing existing kernel build directory"
	echo "    Build directory: ${KERNEL_BUILDDIR}"
	echo "    Set KERNEL_RECONFIG=yes to regenerate it."
fi

# ------------------------------------------------------------------
# Install a makegap.sh that can run on the host
# ------------------------------------------------------------------
#
# The kernel build uses makegap.sh to insert randomized padding into
# the gap.o linker section for KARL (Kernel Address Randomised Link).
# Install the portable implementation and arrange for the Makefile's
# "makegap.sh" target to be satisfied by the pre-existing file.
#
MAKEGAP_TMP="$(mktemp "${KERNEL_BUILDDIR}/makegap.sh.XXXXXX")"
cp "${MAKEGAP_SRC}" "${MAKEGAP_TMP}"
if install_if_changed "${MAKEGAP_TMP}" "${KERNEL_BUILDDIR}/makegap.sh"; then
	echo "    Installed host-compatible makegap.sh"
fi

# Prevent the Makefile's "makegap.sh:" target from overwriting our
# patched version by adding a no-op override via MAKECONF.
KERNEL_MKCONF_TMP="$(mktemp "${KERNEL_BUILDDIR}/Makefile.mk.conf.XXXXXX")"
cat > "${KERNEL_MKCONF_TMP}" <<EOF
makegap.sh:
	@true
EOF
if install_if_changed "${KERNEL_MKCONF_TMP}" "${KERNEL_MKCONF}"; then
	echo "    Installed kernel make override: ${KERNEL_MKCONF}"
fi

# ------------------------------------------------------------------
# Build the kernel
# ------------------------------------------------------------------
echo ""
echo "==> Building kernel (target: bsd)"

# The kernel Makefile defaults to 'all' (which depends on 'bsd').
# Run with -j for parallel builds; the kernel tree is large.
env ${MAKE_ENV} "${BMAKE}" ${MAKE_ARGS} \
	-C "${KERNEL_BUILDDIR}" \
	all

# ------------------------------------------------------------------
# Report results
# ------------------------------------------------------------------
KERNEL_BIN="${KERNEL_BUILDDIR}/bsd"
if [ -f "${KERNEL_BUILDDIR}/bsd.gdb" ]; then
	KERNEL_DBG="${KERNEL_BUILDDIR}/bsd.gdb"
fi

echo ""
echo "==> Kernel build complete"
echo "    Kernel:       ${KERNEL_BIN}"
if [ -n "${KERNEL_DBG:-}" ]; then
	echo "    Debug kernel: ${KERNEL_DBG}"
fi
ls -lh "${KERNEL_BIN}"
file "${KERNEL_BIN}" 2>/dev/null || true

# ------------------------------------------------------------------
# Build bootloaders (stand/)
# ------------------------------------------------------------------
#
# rdboot needs OpenBSD host libraries, while vmboot additionally needs
# rdsetroot and makefs.  Build the remaining BIOS, CD, PXE, and EFI loaders.
OBJCOPY="${OBJCOPY:-${KERNEL_BINDIR}/openbsd-efi-objcopy}"
case "${OBJCOPY}" in
*/*)
	bootstrap_need_exec "${OBJCOPY}"
	;;
*)
	bootstrap_need_tool "${OBJCOPY}"
	;;
esac

STAND_S="${SRCDIR}/sys"
STAND_SADIR="${SRCDIR}/sys/arch/${MACHINE}/stand"
STAND_MAKEOBJDIR="${STAND_MAKEOBJDIR:-obj.cross.${MACHINE}}"
STAND_MAKE_ENV="
	BSDSRCDIR=${SRCDIR}
	BSDOBJDIR=${OBJROOT}
	MAKEOBJDIR=${STAND_MAKEOBJDIR}
	MACHINE=${MACHINE}
	MACHINE_ARCH=${MACHINE_ARCH}
	MACHINE_CPU=${MACHINE_CPU}
	COMPILER_VERSION=clang
	LINKER_VERSION=lld
	WARNINGS=no
	NOMAN=1
	NOPROFILE=1
	LIBCRT0=
	CRTBEGIN=
	CRTEND=
	SOFTRAID=yes
	S=${STAND_S}
	SADIR=${STAND_SADIR}
	MAKECONF=/dev/null
	MAKE=${BMAKE}
	CC=${KERNEL_BINDIR}/openbsd-kernel-cc
	LD=${LLD}
	OBJCOPY=${OBJCOPY}
	SIZE=${SIZE}
	HOSTCC=${HOST_CC}
	BUILDUSER=${HOST_USER}
	BINOWN=${HOST_USER}
	BINGRP=${HOST_GROUP}
	LIBOWN=${HOST_USER}
	LIBGRP=${HOST_GROUP}
"

build_stand_dir() {
	dir="$1"
	shift
	if [ "$#" -eq 0 ]; then
		set -- all
	fi

	echo ""
	echo "==> Building stand/${dir}"
	env ${STAND_MAKE_ENV} "${BMAKE}" ${MAKE_ARGS} \
		-C "${STAND_SADIR}/${dir}" obj
	env ${STAND_MAKE_ENV} "${BMAKE}" ${MAKE_ARGS} \
		-C "${STAND_SADIR}/${dir}" \
		"LD=${LLD}" "SIZE=${SIZE}" "OBJCOPY=${OBJCOPY}" "$@"
}

echo ""
echo "==> Cross-building OpenBSD bootloaders (stand/)"
echo "    CC            = ${KERNEL_BINDIR}/openbsd-kernel-cc"
echo "    LD            = ${LLD}"
echo "    OBJCOPY       = ${OBJCOPY}"

build_stand_dir mbr
build_stand_dir cdbr
build_stand_dir biosboot
build_stand_dir boot all boot.bin
build_stand_dir cdboot
build_stand_dir fdboot
build_stand_dir pxeboot
build_stand_dir efiboot

# ------------------------------------------------------------------
# Collect bootloader outputs
# ------------------------------------------------------------------
STAND_OUTDIR="${OBJROOT}/sys/arch/${MACHINE}/stand"
STAND_COLLECT="${OBJDIR}/stand-${MACHINE}"
mkdir -p "${STAND_COLLECT}"

collect_stand_output() {
	src="$1"
	dst="$2"
	fullsrc="${STAND_OUTDIR}/${src}/${src}"

	case "${src}" in
	boot)
		if [ -f "${fullsrc}" ]; then
			cp "${fullsrc}" "${STAND_COLLECT}/${dst}.elf"
			echo "    ${dst}.elf"
		fi
		if [ -f "${STAND_OUTDIR}/${src}/boot.bin" ]; then
			cp "${STAND_OUTDIR}/${src}/boot.bin" \
			    "${STAND_COLLECT}/${dst}"
			echo "    ${dst} (raw binary)"
		fi
		;;
	efiboot)
		for efi in bootx64 bootia32; do
			case "${efi}" in
			bootx64)
				efiname=BOOTX64.EFI
				;;
			bootia32)
				efiname=BOOTIA32.EFI
				;;
			esac
			efidir="${STAND_OUTDIR}/${src}/${efi}"
			if [ -f "${efidir}/${efiname}" ]; then
				cp "${efidir}/${efiname}" \
				    "${STAND_COLLECT}/${efiname}"
				echo "    ${efiname}"
			fi
		done
		;;
	*)
		if [ -f "${fullsrc}" ]; then
			cp "${fullsrc}" "${STAND_COLLECT}/${dst}"
			echo "    ${dst}"
		fi
		;;
	esac
}

echo ""
echo "==> Collecting bootloader outputs to ${STAND_COLLECT}"
collect_stand_output mbr mbr
collect_stand_output cdbr cdbr
collect_stand_output biosboot biosboot
collect_stand_output boot boot
collect_stand_output cdboot cdboot
collect_stand_output fdboot fdboot
collect_stand_output pxeboot pxeboot
collect_stand_output efiboot ""

echo ""
echo "==> Bootloader build complete"
ls -lh "${STAND_COLLECT}/"
