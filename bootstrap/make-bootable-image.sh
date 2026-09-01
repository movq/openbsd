#!/bin/sh
#
# make-bootable-image.sh - create an amd64 UEFI-bootable OpenBSD disk image
# from the GNU/Linux bootstrap outputs.
#
# Usage:
#   sudo ./bootstrap/make-bootable-image.sh
#
# Useful environment overrides:
#   IMAGE=...              output disk image
#   DESTDIR=...            staged OpenBSD userland, default bootstrap/dest
#   DISK_SIZE=...          truncate(1) size, default auto MiB size
#   ESP_SIZE_MIB=...       EFI System Partition size, default 260
#   ROOT_HEADROOM_MIB=...  extra FFS space beyond staged contents, default 1536
#   FFS_SIZE_MIB=...       exact FFS image size, overrides auto sizing
#   ROOT_DUID=...          16 hex digits for the root disklabel DUID
#   DUID_FSTAB=0           keep staged /etc/fstab instead of using ROOT_DUID.a
#   OVERWRITE=1            allow replacing an existing IMAGE
#   KEEP_WORKDIR=1         keep temporary staging and FFS image
#
set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"

MACHINE="${MACHINE:-amd64}"
DESTDIR="${DESTDIR:-${SCRIPTDIR}/dest}"
IMAGE="${IMAGE:-${SCRIPTDIR}/openbsd-${MACHINE}-efi.img}"
MAKEFS="${MAKEFS:-${SCRIPTDIR}/tools/bin/makefs}"
KERNEL="${KERNEL:-${SCRIPTDIR}/obj-host/sys/arch/${MACHINE}/compile/GENERIC.MP/bsd}"
EFIBOOT="${EFIBOOT:-${SCRIPTDIR}/obj-host/sys/arch/${MACHINE}/stand/efiboot/bootx64/BOOTX64.EFI}"

ESP_SIZE_MIB="${ESP_SIZE_MIB:-260}"
ROOT_HEADROOM_MIB="${ROOT_HEADROOM_MIB:-131072}"
WORKDIR_PARENT="${WORKDIR_PARENT:-${SCRIPTDIR}/build}"

LOOPDEV=
ESPMOUNT=
WORKDIR=

die() {
	echo "$*" >&2
	exit 1
}

need_exec() {
	if [ ! -x "$1" ]; then
		die "missing required executable: $1"
	fi
}

need_file() {
	if [ ! -f "$1" ]; then
		die "missing required file: $1"
	fi
}

need_tool() {
	if ! command -v "$1" >/dev/null 2>&1; then
		die "missing required host tool: $1"
	fi
}

cleanup() {
	if [ -n "${ESPMOUNT}" ] && mountpoint -q "${ESPMOUNT}" 2>/dev/null; then
		umount "${ESPMOUNT}" || true
	fi
	if [ -n "${LOOPDEV}" ]; then
		losetup -d "${LOOPDEV}" 2>/dev/null || true
	fi
	if [ -n "${WORKDIR}" ] && [ "${KEEP_WORKDIR:-0}" != 1 ]; then
		rm -rf "${WORKDIR}"
	fi
}
trap cleanup EXIT HUP INT TERM

partdev() {
	case "$1" in
		/dev/loop[0-9]*) printf '%sp%s\n' "$1" "$2" ;;
		*) printf '%s%s\n' "$1" "$2" ;;
	esac
}

wait_for_partitions() {
	n=0
	while [ "${n}" -lt 50 ]; do
		if [ -b "${ESPDEV}" ] && [ -b "${OPENBSDDEV}" ]; then
			return
		fi
		sleep 0.1
		n=$((n + 1))
	done
}

partition_start_lba() {
	devbase="$(basename "$1")"
	start="/sys/class/block/${devbase}/start"
	if [ ! -r "${start}" ]; then
		die "cannot determine partition start LBA from ${start}"
	fi
	cat "${start}"
}

partition_size_lba() {
	devbase="$(basename "$1")"
	size="/sys/class/block/${devbase}/size"
	if [ ! -r "${size}" ]; then
		die "cannot determine partition size from ${size}"
	fi
	cat "${size}"
}

generate_duid() {
	od -An -N8 -tx1 -v /dev/urandom | tr -d ' \n'
}

validate_duid() {
	case "$1" in
		????????????????) ;;
		*) die "ROOT_DUID must be exactly 16 lowercase hex digits" ;;
	esac
	case "$1" in
		*[!0123456789abcdef]*) die "ROOT_DUID must be exactly 16 lowercase hex digits" ;;
	esac
}

devspec_node() {
	name="$1"
	type="$2"
	major="$3"
	minor="$4"
	mode="$5"

	printf '/dev/%s %s %s %s 0%s 0 0\n' \
	    "${name}" "${type}" "${major}" "${minor}" "${mode}" >> "${DEV_SPEC}"
}

devspec_disk_nodes() {
	prefix="$1"
	blk_major="$2"
	chr_major="$3"

	devspec_node "${prefix}0a" b "${blk_major}" 0 640
	devspec_node "${prefix}0c" b "${blk_major}" 2 640
	devspec_node "r${prefix}0a" c "${chr_major}" 0 640
	devspec_node "r${prefix}0c" c "${chr_major}" 2 640
}

write_dev_spec() {
	: > "${DEV_SPEC}"
	mkdir -p "${STAGING}/dev"

	devspec_node console c 0 0 600
	devspec_node tty c 1 0 666
	devspec_node null c 2 2 666
	devspec_node zero c 2 12 666
	devspec_node stdin c 22 0 666
	devspec_node stdout c 22 1 666
	devspec_node stderr c 22 2 666
	devspec_node klog c 7 0 600
	devspec_node urandom c 45 0 644
	rm -f "${STAGING}/dev/random"
	ln -s urandom "${STAGING}/dev/random"

	devspec_node ttyC0 c 12 0 600
	devspec_node tty00 c 8 0 660
	devspec_node tty01 c 8 1 660

	devspec_disk_nodes wd 0 3
	devspec_disk_nodes sd 4 13
}

create_gpt() {
	fdisk "${IMAGE}" <<EOF
g
n
1

+${ESP_SIZE_MIB}M
t
1
n
2


t
2
203
w
EOF
}

if [ "$1" = "makedevspec" ]; then
	STAGING=bootstrap/obj/dest-amd64
	DEV_SPEC=bootstrap/obj/dev.spec
	write_dev_spec
	exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
	die "this script must be run as root"
fi

need_tool truncate
need_tool fdisk
need_tool losetup
need_tool mkfs.vfat
need_tool mount
need_tool umount
need_tool dd
need_tool du
need_tool awk
need_tool mktemp
need_tool blockdev
need_tool mountpoint
need_tool stat
need_tool sleep
need_tool basename
need_tool cat
need_tool od
need_tool tr
need_exec "${MAKEFS}"
need_file "${KERNEL}"
need_file "${EFIBOOT}"

case "$(fdisk --version 2>/dev/null || true)" in
	*util-linux*) ;;
	*) die "fdisk does not appear to be util-linux fdisk" ;;
esac

if [ ! -d "${DESTDIR}" ]; then
	die "missing staged DESTDIR: ${DESTDIR}"
fi
if [ -e "${IMAGE}" ] && [ ! -f "${IMAGE}" ]; then
	die "refusing to use non-regular image path: ${IMAGE}"
fi
if [ -f "${IMAGE}" ] && [ "${OVERWRITE:-0}" != 1 ]; then
	die "refusing to overwrite existing image: ${IMAGE} (set OVERWRITE=1)"
fi

mkdir -p "${WORKDIR_PARENT}"
WORKDIR="$(mktemp -d "${WORKDIR_PARENT}/bootable-image.XXXXXXXXXX")"
STAGING="${WORKDIR}/staging"
FFS_IMAGE="${WORKDIR}/openbsd-root.ffs"
DEV_SPEC="${WORKDIR}/devspec"
ESPMOUNT="${WORKDIR}/esp"
ROOT_DUID="${ROOT_DUID:-$(generate_duid)}"
validate_duid "${ROOT_DUID}"

echo "==> Staging OpenBSD root filesystem"
mkdir -p "${STAGING}"
cp -a "${DESTDIR}/." "${STAGING}/"
cp "${KERNEL}" "${STAGING}/bsd"
chmod 644 "${STAGING}/bsd"
write_dev_spec
if [ "${DUID_FSTAB:-1}" != 0 ]; then
	mkdir -p "${STAGING}/etc"
	printf '%s.a / ffs rw 1 1\n' "${ROOT_DUID}" > "${STAGING}/etc/fstab"
	chmod 644 "${STAGING}/etc/fstab"
fi
chown -R 0:0 "${STAGING}"

apparent_mib="$(du -sm --apparent-size "${STAGING}" | awk '{ print $1 }')"
if [ -n "${FFS_SIZE_MIB:-}" ]; then
	root_size_mib="${FFS_SIZE_MIB}"
else
	root_size_mib="$((apparent_mib + ROOT_HEADROOM_MIB))"
fi

if [ -n "${DISK_SIZE:-}" ]; then
	disk_size="${DISK_SIZE}"
else
	disk_size_mib="$((root_size_mib + ESP_SIZE_MIB + 64))"
	disk_size="${disk_size_mib}M"
fi

echo "    DESTDIR         = ${DESTDIR}"
echo "    kernel          = ${KERNEL}"
echo "    EFI loader      = ${EFIBOOT}"
echo "    root DUID       = ${ROOT_DUID}"
echo "    staged apparent = ${apparent_mib} MiB"
echo "    FFS size        = ${root_size_mib} MiB"
echo "    disk image      = ${IMAGE}"
echo "    disk size       = ${disk_size}"

echo "==> Creating sparse disk image and GPT"
rm -f "${IMAGE}"
truncate -s "${disk_size}" "${IMAGE}"
create_gpt

echo "==> Attaching loop device"
LOOPDEV="$(losetup --find --show -P "${IMAGE}")"
ESPDEV="$(partdev "${LOOPDEV}" 1)"
OPENBSDDEV="$(partdev "${LOOPDEV}" 2)"
wait_for_partitions

if [ ! -b "${ESPDEV}" ] || [ ! -b "${OPENBSDDEV}" ]; then
	die "loop partitions did not appear: ${ESPDEV} ${OPENBSDDEV}"
fi
OPENBSD_START_LBA="$(partition_start_lba "${OPENBSDDEV}")"
OPENBSD_SECTORS="$(partition_size_lba "${OPENBSDDEV}")"
DISK_SECTORS="$(blockdev --getsz "${LOOPDEV}")"

echo "==> Creating and populating EFI System Partition"
mkfs.vfat -F 32 -n EFI "${ESPDEV}"
mkdir -p "${ESPMOUNT}"
mount "${ESPDEV}" "${ESPMOUNT}"
mkdir -p "${ESPMOUNT}/EFI/BOOT"
cp "${EFIBOOT}" "${ESPMOUNT}/EFI/BOOT/BOOTX64.EFI"
sync
umount "${ESPMOUNT}"

echo "==> Building FFS root image with OpenBSD disklabel"
echo "    OpenBSD start LBA = ${OPENBSD_START_LBA}"
echo "    OpenBSD sectors   = ${OPENBSD_SECTORS}"
"${MAKEFS}" -t ffs -s "${root_size_mib}m" \
    -D "${DEV_SPEC}" \
    -o "openbsdlabel=1,minfree=0,bsize=16384,fsize=2048,density=8192,disksectors=${DISK_SECTORS},openbsdstart=${OPENBSD_START_LBA},openbsdsectors=${OPENBSD_SECTORS},duid=${ROOT_DUID},sparse" \
    "${FFS_IMAGE}" "${STAGING}"

ffs_bytes="$(stat -c %s "${FFS_IMAGE}")"
openbsd_bytes="$(blockdev --getsize64 "${OPENBSDDEV}")"
if [ "${ffs_bytes}" -gt "${openbsd_bytes}" ]; then
	die "FFS image (${ffs_bytes} bytes) is larger than OpenBSD partition (${openbsd_bytes} bytes)"
fi

echo "==> Writing FFS image to OpenBSD partition"
dd if="${FFS_IMAGE}" of="${OPENBSDDEV}" bs=4M conv=fsync,sparse status=progress
sync

echo "==> Detaching loop device"
losetup -d "${LOOPDEV}"
LOOPDEV=

echo "==> Created ${IMAGE}"
if [ "${KEEP_WORKDIR:-0}" = 1 ]; then
	echo "    kept work directory: ${WORKDIR}"
fi
