#!/bin/sh
#
# cross-build-kernel.sh - cross-build an OpenBSD kernel on GNU/Linux.
#
# Uses bootstrap/tools/bin/config (already built by build-tools.sh) to
# generate a kernel build directory from a config file, then invokes
# bmake with the cross-compiler toolchain.
#
# Usage:
#   ./bootstrap/cross-build-kernel.sh [MACHINE=amd64] [CONFIG=GENERIC.MP]
#
set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "${SCRIPTDIR}/.." && pwd)"

BMAKE="${BMAKE:-${SCRIPTDIR}/tools/bin/bmake}"
TOOLDIR="${TOOLDIR:-${SCRIPTDIR}/tools}"
CONFIG_BIN="${CONFIG_BIN:-${TOOLDIR}/bin/config}"
OBJROOT="${OBJROOT:-${SCRIPTDIR}/obj-host}"

MACHINE="${MACHINE:-amd64}"
MACHINE_ARCH="${MACHINE_ARCH:-amd64}"
MACHINE_CPU="${MACHINE_CPU:-${MACHINE_ARCH}}"
TARGET_CANON="${TARGET_CANON:-${MACHINE_ARCH}-unknown-openbsd7.9}"
KERNEL_CONFIG="${KERNEL_CONFIG:-GENERIC.MP}"

CLANG="${CLANG:-${TOOLDIR}/bin/clang}"
LLD="${LLD:-${TOOLDIR}/bin/ld.lld}"
if [ -n "${HOSTCC:-}" ]; then
	HOST_CC="${HOSTCC}"
elif [ -x /usr/bin/cc ]; then
	HOST_CC="/usr/bin/cc"
else
	HOST_CC="cc"
fi
HOST_OBJCOPY="${HOST_OBJCOPY:-}"
if [ -z "${HOST_OBJCOPY}" ]; then
	HOST_OBJCOPY="$(command -v objcopy 2>/dev/null || true)"
fi

BUILDUSER="${BUILDUSER:-$(id -un)}"
BUILDGROUP="${BUILDGROUP:-$(id -gn)}"

WRAPDIR="${SCRIPTDIR}/wrap"
KERNEL_BINDIR="${SCRIPTDIR}/kernel-tools/bin"

need_tool() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "missing required host tool: $1" >&2
		exit 1
	fi
}

need_file() {
	if [ ! -f "$1" ]; then
		echo "missing required file: $1" >&2
		exit 1
	fi
}

need_exec() {
	if [ ! -x "$1" ]; then
		echo "missing required executable: $1" >&2
		exit 1
	fi
}

install_if_changed() {
	local src="$1"
	local dst="$2"

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
need_exec "${BMAKE}"
need_exec "${CLANG}"
need_exec "${LLD}"
need_exec "${CONFIG_BIN}"
need_tool install
need_tool sort
need_tool mktemp
if [ -z "${OBJCOPY:-}" ]; then
	if [ -z "${HOST_OBJCOPY}" ]; then
		echo "missing required host tool: objcopy" >&2
		exit 1
	fi
	need_exec "${HOST_OBJCOPY}"
fi

# ------------------------------------------------------------------
# Create tool wrappers
# ------------------------------------------------------------------
mkdir -p "${KERNEL_BINDIR}"

# Cross-compiler wrapper: kernel compilation uses -nostdinc and
# -ffreestanding already, so --sysroot is unnecessary.  Just set
# the target triple so clang emits amd64 machine code.
cat > "${KERNEL_BINDIR}/openbsd-kernel-cc" <<EOF
#!/bin/sh
compile_only=no
for arg do
	case "\$arg" in
		-c|-S|-E|-M|-MM) compile_only=yes ;;
	esac
done
if [ "\$compile_only" = yes ]; then
	exec "${CLANG}" --target="${TARGET_CANON}" "\$@"
fi
exec "${CLANG}" --target="${TARGET_CANON}" -fuse-ld="${LLD}" "\$@"
EOF
chmod +x "${KERNEL_BINDIR}/openbsd-kernel-cc"

# OpenBSD kernels use ctfstrip(1) to convert debug info into CTF.
# That tool does not exist on Linux.  Replace it with a no-op so the
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
	cat > "${KERNEL_BINDIR}/openbsd-efi-objcopy" <<'EOF'
#!/bin/sh

if [ -n "${HOST_OBJCOPY:-}" ]; then
	real_objcopy="${HOST_OBJCOPY}"
elif [ -x /usr/bin/objcopy ]; then
	real_objcopy=/usr/bin/objcopy
elif [ -x /bin/objcopy ]; then
	real_objcopy=/bin/objcopy
else
	real_objcopy=objcopy
fi

efi_format=
for arg do
	case "${arg}" in
	--target=efi-app-x86_64|--target=efi-app-ia32)
		efi_format="${arg#--target=}"
		;;
	efi-app-x86_64|efi-app-ia32)
		efi_format="${arg}"
		;;
	esac
done

if [ -n "${efi_format}" ]; then
	while [ $# -gt 2 ]; do
		shift
	done
	exec "${real_objcopy}" \
	    -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel \
	    -j .rel.dyn -j .rela -j .rela.dyn -j .reloc \
	    -O "${efi_format}" "$1" "$2"
fi

exec "${real_objcopy}" "$@"
EOF
	chmod +x "${KERNEL_BINDIR}/openbsd-efi-objcopy"
fi

# Prepend kernel wrapper directory to PATH.
export PATH="${KERNEL_BINDIR}:${TOOLDIR}/bin:${WRAPDIR}:${PATH}"
export MACHINE MACHINE_ARCH MACHINE_CPU

# Kernel build directory determined early so we can set up MAKECONF.
CONFIG_SRC="${SRCDIR}/sys/arch/${MACHINE}/conf/${KERNEL_CONFIG}"
KERNEL_BUILDDIR="${OBJROOT}/sys/arch/${MACHINE}/compile/${KERNEL_CONFIG}"
KERNEL_MKCONF="${KERNEL_BUILDDIR}/Makefile.mk.conf"

# ------------------------------------------------------------------
# bmake arguments
# ------------------------------------------------------------------
MAKE_ARGS="-m ${SRCDIR}/share/mk"

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
	SIZE=size
	HOSTCC=${HOST_CC}
	BINOWN=${BUILDUSER}
	BINGRP=${BUILDGROUP}
	LIBOWN=${BUILDUSER}
	LIBGRP=${BUILDGROUP}
	SHAREOWN=${BUILDUSER}
	SHAREGRP=${BUILDGROUP}
	MANOWN=${BUILDUSER}
	MANGRP=${BUILDGROUP}
"

# ------------------------------------------------------------------
# Run config to generate the kernel build directory
# ------------------------------------------------------------------
need_file "${CONFIG_SRC}"

echo "==> Cross-building OpenBSD kernel"
echo "    SRCDIR        = ${SRCDIR}"
echo "    BMAKE         = ${BMAKE}"
echo "    CONFIG        = ${CONFIG_BIN}"
echo "    TARGET        = ${TARGET_CANON}"
echo "    MACHINE       = ${MACHINE}"
echo "    MACHINE_ARCH  = ${MACHINE_ARCH}"
echo "    KERNEL_CONFIG = ${KERNEL_CONFIG}"
echo "    BUILDDIR      = ${KERNEL_BUILDDIR}"
echo "    CC            = ${KERNEL_BINDIR}/openbsd-kernel-cc"
echo "    LD            = ${LLD}"
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
# Patch makegap.sh for Linux compatibility
# ------------------------------------------------------------------
#
# The kernel build uses makegap.sh to insert randomised padding into
# the gap.o linker section for KARL (Kernel Address Randomised Link).
# The script uses sysctl(8) and jot(1), neither of which is portable
# to Linux.  Replace the generated copy with a portable version that
# uses getconf(1) and $RANDOM, and arrange for the Makefile's
# "makegap.sh" target to be satisfied by the pre-existing file.
#
MAKEGAP_TMP="$(mktemp "${KERNEL_BUILDDIR}/makegap.sh.XXXXXX")"
cat > "${MAKEGAP_TMP}" <<'MAKEGAP_EOF'
#!/bin/sh -
random_uniform() {
	local _upper_bound
	if [ "$1" -gt 0 ]; then
		_upper_bound=$(($1 - 1))
	else
		_upper_bound=0
	fi
	if [ "$1" -gt 0 ]; then
		echo $((RANDOM % $1))
	else
		echo 0
	fi
}

umask 007

if PAGE_SIZE=$(getconf PAGESIZE 2>/dev/null) && [ -n "$PAGE_SIZE" ]; then
	:
elif PAGE_SIZE=$(sysctl -n hw.pagesize 2>/dev/null) && [ -n "$PAGE_SIZE" ]; then
	:
else
	PAGE_SIZE=4096
fi
PAD=$1
GAPDUMMY=$2

RANDOM1=`random_uniform $((3 * PAGE_SIZE))`
RANDOM2=`random_uniform $PAGE_SIZE`
RANDOM3=`random_uniform $PAGE_SIZE`
RANDOM4=`random_uniform $PAGE_SIZE`
RANDOM5=`random_uniform $PAGE_SIZE`

cat > gap.link << __EOF__

PHDRS {
	text PT_LOAD FILEHDR PHDRS;
	rodata PT_LOAD;
	data PT_LOAD;
	bss PT_LOAD;
}

SECTIONS {
	.text : ALIGN($PAGE_SIZE) {
		LONG($PAD);
		. += $RANDOM1;
		. = ALIGN($PAGE_SIZE);
		endboot = .;
		PROVIDE (endboot = .);
		. = ALIGN($PAGE_SIZE);
		. += $RANDOM2;
		. = ALIGN(16);
		*(.text .text.*)
	} :text =$PAD

	.rodata : {
		LONG($PAD);
		. += $RANDOM3;
		. = ALIGN(16);
		*(.rodata .rodata.*)
	} :rodata =$PAD

	.data : {
		LONG($PAD);
		. = . + $RANDOM4;	/* fragment of page */
		. = ALIGN(16);
		*(.data .data.*)
	} :data =$PAD

	.bss : {
		. = . + $RANDOM5;	/* fragment of page */
		. = ALIGN(16);
		*(.bss .bss.*)
	} :bss
}
__EOF__

$LD $LDFLAGS -r gap.link $GAPDUMMY -o gap.o
MAKEGAP_EOF
if install_if_changed "${MAKEGAP_TMP}" "${KERNEL_BUILDDIR}/makegap.sh"; then
	echo "    Installed Linux-compatible makegap.sh"
fi
chmod +x "${KERNEL_BUILDDIR}/makegap.sh"

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
# The stand/ tree builds BIOS and EFI bootloaders.  Most use -m32
# (32-bit x86) and link with ld -melf_i386.  The existing cross-clang
# handles -m32 natively; ld.lld handles -melf_i386.
#
# We skip:
#   rdboot  – needs -lutil -lz (OpenBSD host libraries)
#   vmboot  – needs rdboot + VMBOOT kernel + makefs + rdsetroot
#
# The boot Makefile keeps the ELF around and can also produce a
# companion boot.bin raw binary.  We request that explicitly for boot.
#
# check-boot.pl (used by boot/) invokes /usr/bin/objdump; on Linux
# this is the host objdump which can read OpenBSD ELF headers fine.
#

need_tool perl
need_tool objdump
need_tool size

OBJCOPY="${OBJCOPY:-${KERNEL_BINDIR}/openbsd-efi-objcopy}"
case "${OBJCOPY}" in
*/*) need_exec "${OBJCOPY}" ;;
*) need_tool "${OBJCOPY}" ;;
esac
OBJDUMP="objdump"
SIZE="size"

STAND_S="${SRCDIR}/sys"
STAND_SADIR="${SRCDIR}/sys/arch/amd64/stand"
STAND_MAKEOBJDIR="obj.linux.${MACHINE}"

# Stand build uses the same CC wrapper as the kernel (it adds
# --target=${TARGET_CANON} and -fuse-ld=${LLD} for linking).
# For genassym.sh invocations, the CC must be the raw clang binary
# rather than the kernel wrapper (since genassym.sh runs it as a
# plain compiler, not a linker).  We use the kernel CC wrapper for
# normal compilation and let Makefile rules drive it.
#
# Override LD to use ld.lld for all linking (the Makefiles default
# to "ld" which is OpenBSD's ld.lld on native builds).

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
	MAKE=${BMAKE}
	CC=${KERNEL_BINDIR}/openbsd-kernel-cc
	LD=${LLD}
	OBJCOPY=${OBJCOPY}
	SIZE=${SIZE}
	HOSTCC=${HOST_CC}
	BINOWN=${BUILDUSER}
	BINGRP=${BUILDGROUP}
	LIBOWN=${BUILDUSER}
	LIBGRP=${BUILDGROUP}
"

build_stand_dir() {
	local dir="$1"
	local target="${2:-all}"

	echo ""
	echo "==> Building stand/${dir}"
	env ${STAND_MAKE_ENV} "${BMAKE}" ${MAKE_ARGS} \
		-C "${STAND_SADIR}/${dir}" \
		obj
	env ${STAND_MAKE_ENV} "${BMAKE}" ${MAKE_ARGS} \
		-C "${STAND_SADIR}/${dir}" \
		"LD=${LLD}" "SIZE=${SIZE}" "OBJCOPY=${OBJCOPY}" \
		${target}
}

echo ""
echo "==> Cross-building OpenBSD bootloaders (stand/)"
echo "    MACHINE       = ${MACHINE}"
echo "    CC            = ${KERNEL_BINDIR}/openbsd-kernel-cc"
echo "    LD            = ${LLD}"
echo "    OBJCOPY       = ${OBJCOPY}"
echo ""

# mbr – Master Boot Record (assembly + objcopy -O binary)
build_stand_dir mbr

# cdbr – El Torito CD boot record (assembly + objcopy -O binary)
build_stand_dir cdbr

# biosboot – BIOS bootstrap block (assembly, linked with ld.script)
build_stand_dir biosboot

# boot – Second-stage BIOS bootloader (C + assembly, 32-bit).
# Also build boot.bin (raw binary companion).
build_stand_dir boot "all boot.bin"

# cdboot – CD bootloader (32-bit, produces raw binary inline)
build_stand_dir cdboot

# fdboot – Floppy-disk variant of boot(8).  Same sources, -DFDBOOT.
# Do not request boot.bin here: the inherited rule depends on a literal
# target named "boot", which triggers sys.mk's generic boot.c link rule.
build_stand_dir fdboot

# pxeboot – PXE network bootloader (32-bit, raw binary inline)
build_stand_dir pxeboot

# efiboot – EFI bootloaders: bootx64 (BOOTX64.EFI) and bootia32
# (BOOTIA32.EFI).  Linked as shared objects, then objcopy'd
# to PE/EFI images.  Subdir recursion handles both variants.
build_stand_dir efiboot

# ------------------------------------------------------------------
# Collect bootloader outputs
# ------------------------------------------------------------------

STAND_OUTDIR="${OBJROOT}/sys/arch/amd64/stand"
STAND_COLLECT="${SCRIPTDIR}/obj-host/stand"
mkdir -p "${STAND_COLLECT}"

collect_stand_output() {
	local src="$1"
	local dst="$2"
	local fullsrc="${STAND_OUTDIR}/${src}/${src}"
	local fullsrc2=""

	case "${src}" in
	boot)
		# ELF + binary pair
		if [ -f "${fullsrc}" ]; then
			cp "${fullsrc}" "${STAND_COLLECT}/${dst}.elf"
			echo "    ${dst}.elf"
		fi
		fullsrc2="${STAND_OUTDIR}/${src}/boot.bin"
		if [ -f "${fullsrc2}" ]; then
			cp "${fullsrc2}" "${STAND_COLLECT}/${dst}"
			echo "    ${dst} (raw binary)"
		fi
		;;
	efiboot)
		for efi in bootx64 bootia32; do
			local efidir="${STAND_OUTDIR}/${src}/${efi}"
			local efiname=""
			case "${efi}" in
				bootx64) efiname="BOOTX64.EFI" ;;
				bootia32) efiname="BOOTIA32.EFI" ;;
			esac
			if [ -f "${efidir}/${efiname}" ]; then
				cp "${efidir}/${efiname}" "${STAND_COLLECT}/${efiname}"
				echo "    ${efiname}"
			fi
		done
		;;
	*)
		if [ -f "${fullsrc}" ]; then
			cp "${fullsrc}" "${STAND_COLLECT}/${src}"
			echo "    ${src}"
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
ls -lh "${STAND_COLLECT}/" 2>/dev/null || true
