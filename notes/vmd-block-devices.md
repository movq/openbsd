The existing virtio data path already uses positional preadv(2)/pwritev(2) calls and can operate on a raw disk descriptor once validation and capacity handling are fixed.

Current Blockers

- config_setvm() opens disks with O_RDWR | O_EXLOCK | O_NONBLOCK, but
  vm_checkaccess() explicitly requires S_ISREG() in usr.sbin/vmd/config.c and
  usr.sbin/vmd/vmd.c.
- virtio_raw_init() determines capacity with lseek(fd, 0, SEEK_END) in
  usr.sbin/vmd/vioraw.c. That does not provide the partition capacity for an
  OpenBSD raw disk device.
- Device format auto-detection reads the first bytes and could mistakenly
  treat a device as QCOW2.
- The vioblk process drops to the stdio pledge before backend
  initialization. Querying DIOCGDINFO there would require the disklabel
  promise.
- The virtio implementation assumes 512-byte I/O. A raw device may have a
  native sector size of 4096 bytes and reject 512-byte host operations.
- Existing exclusive locks protect the same special vnode, but do not
  necessarily prevent conflicting access through overlapping partitions, such
  as /dev/rsd0a and /dev/rsd0c, or through host mounts and swap.
- Commit 31749646709 deliberately rejected block and character devices
  because they did not work and produced confusing errors. Its log does not
  describe a security prohibition, so this would effectively complete
  previously missing support.

Implementation Outline

1. Limit support to OpenBSD raw character disk devices such as /dev/rsd1c.
  Do not accept arbitrary character devices or, initially, buffered
  block-device nodes such as /dev/sd1c.
2. Preserve regular-file checking for kernels, CD-ROMs, and image files. Add
  a disk-specific validator rather than broadly weakening vm_checkaccess().
  After opening the descriptor, accept either a regular file or a character
  device for which DIOCGDINFO succeeds.
3. Require raw semantics for device nodes. In vm.conf, document:

  disk "/dev/rsd1c" format raw
For vmctl, document:

  vmctl start -d raw:/dev/rsd1c vmname
Reject VMDF_QCOW2 for special devices; either require explicit raw format or
  turn VMDF_AUTO into raw without probing device contents.
4. Query device metadata with DIOCGDINFO. Use fstat() and
  DISKPART(st.st_rdev) to select the opened partition, then calculate:

  bytes = DL_GETPSIZE(&label.d_partitions[part]) * label.d_secsize;
Validate the partition index, nonzero size, multiplication overflow, off_t
  range, and divisibility by the 512-byte virtio sector size. The device node
  itself handles partition-relative offsets.
5. Prefer doing that query in the vmd parent during startup and passing
  trusted size and sector metadata with the disk descriptor. This gives
  synchronous startup errors and lets the vioblk child remain pledged to
  stdio. It would require internal fields such as per-disk byte size, native
  sector size, and backing kind in struct vmop_create_params or in the disk-fd
  message.
6. The simpler alternative is to query from virtio_raw_init(). That requires
  adding disklabel to the initial pledge in vioblk_main() and delaying the
  reduction to stdio until after initialization. This is less plumbing, but
  exposes the broader disklabel ioctl set to the vioblk process during
  initialization.
7. Change the raw backend state in usr.sbin/vmd/vioraw.c from a bare int *
  to a structure containing the descriptor, backing kind, byte capacity, and
  native sector size. Regular files can retain the current seek-based sizing;
  devices use the validated metadata.
8. Harden vioblk_io() while adding the device support. Check multiplication
  of cmd->sector * 512, reject transfers beyond advertised capacity, reject
  offset + length overflow, and correct the alignment condition, which
  currently uses && where enforcing both aligned offset and aligned length
  requires ||.
9. Decide how to support native sectors larger than 512 bytes. A reasonable
  first patch would reject devices whose d_secsize != 512. Full support would
  advertise VIRTIO_BLK_F_BLK_SIZE, implement the block-size configuration
  register, enforce native alignment against malicious requests, and
  potentially provide serialized read-modify-write handling for guests or
  firmware that still issue 512-byte writes.
10. Retain O_EXLOCK, but add device-identity conflict checks. At minimum,
  reject simultaneous VM use of the same disk unit even through different
  partitions. A more precise implementation could compare partition byte
  ranges derived from disklabel offsets and sizes. Host-mounted filesystems,
  swap, softraid relationships, and non-cooperating host tools cannot be made
  safe by flock(2) alone, so this remains an administrative safety
  requirement.
11. Consider durability separately. vmd currently does not advertise
  VIRTIO_BLK_F_FLUSH; guest cache synchronization therefore does not reach
  either image files or physical disk caches. Proper support would add a
  backing flush callback, use fsync() for files and DIOCCACHESYNC for raw
  devices, and advertise/handle VIRTIO_BLK_T_FLUSH. Device cache sync would
  require retaining or delegating the disklabel privilege.
12. Add clear error reporting for unsupported special devices, unavailable
  partitions, incompatible sector sizes, lock conflicts, and permission
  failures. Continue checking access against the requesting VM owner after
  opening the descriptor to preserve the current anti-TOCTOU design. A policy
  decision is also needed on whether ad hoc device assignment is root-only
  while administrator-defined VMs may use configured devices.

Tests And Documentation

- Extend the regress/usr.sbin/vmd coverage with a disposable vnd(4) image
  exposed through /dev/rvndNc, avoiding tests against real disks.
- Verify whole-device and individual-partition capacity, guest read/write
  correctness, persistence after shutdown, reboot behavior, lock release, and
  unchanged raw/QCOW2 file behavior.
- Add negative tests for /dev/null, QCOW2 format on a device, nonexistent or
  zero-sized partitions, permission denial, duplicate/overlapping device use,
  out-of-range virtio requests, and sector arithmetic overflow.
- Test 4K-sector behavior according to the chosen policy, plus short I/O,
  device removal/error propagation, and cache flushing if implemented.
- Update vm.conf(5), vmctl(8), and likely vmd(8) to say “raw character disk
  device,” require exclusive host ownership, and warn that assigning a
  mounted, swap, boot, or softraid member disk can corrupt the host.

No changes should be required under sys/dev/vmm or in the
architecture-specific VMM code unless the scope expands beyond the existing
userland virtio-block emulation.
