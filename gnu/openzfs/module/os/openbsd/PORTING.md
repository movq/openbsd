# OpenZFS/OpenBSD kernel port

OpenZFS is compiled in place.  `sys/zfs/files.zfs` owns the object list and
`ZFS_C` owns the common compiler and include flags.  The configured amd64
kernel now compiles and links with ZFS enabled.

## Status

| Subsystem | Status |
| --- | --- |
| SPL | Core allocation, synchronization, taskq, thread, TSD, kstat, UIO, XDR, and utility compatibility is present. |
| Checksums, compression, ABD, RAID-Z | Portable implementations compile.  SIMD/assembly backends and caller-page-backed ABDs are deferred. |
| Vdev and storage | Portable topology, labels, queues, disk and file I/O, mirrors, RAID-Z, dRAID, indirect vdevs, removal, rebuild, initialize, and trim compile. |
| SPA and allocator | Metaslab, space-map, SPA lifecycle/configuration/checkpoint/history/statistics/error-log, MMP, DDT/BRT, traversal, and TXG units compile.  Native pool cachefile I/O is present. |
| ARC | Portable ARC and the initial OpenBSD memory policy compile. |
| DMU, ZAP, and SA | Object, objset, transaction, dbuf, dnode, zfetch, ZAP, SA, direct-DMU, send, and receive units compile. |
| DSL | Pool, dataset, directory, property, sync-task, deadlist, bookmark, delegation, destroy, hold, scan, and encryption-metadata units compile.  Native ZFS encryption is intentionally unsupported. |
| ZIL | The common engine, log construction, replay dispatcher, and namespace/write/basic-attribute replay paths compile.  Extended attributes remain deferred. |
| VFS and ZPL | The static kernel runtime supports dataset creation, mount/unmount, statfs/sync, ZIL replay and unlinked-set recovery, znodes, lookup/enumeration, mode-bit access, regular-file and FIFO I/O, attribute/size changes, and namespace mutation.  Remount, online rollback/receive, and live property callbacks are deferred. |
| Control plane | The full current OpenZFS ioctl command table, `/dev/zfs` transport, per-open state, on-exit cleanup, event queues, and kernel pool config/stats/try-import/import path compile.  Native makefiles now build and link `zpool`, `zfs`, `zinject`, and `zfs_ids_to_path` with their private static libraries.  Runtime validation and user-visible tunables remain incomplete. |
| Other deferred work | Zvol devices, extended attributes, channel programs, VM/pager integration, and native ZFS encryption. |

`KERNEL_RECONFIG=no JOBS=8 ./bootstrap/kernel-make.sh` produces a linked
`GENERIC.MP` kernel on the Linux cross-build host.

The shared module/header scan currently finds these compatibility headers
without an OpenBSD or common implementation:

```text
sys/freebsd_crypto.h     sys/pcpu.h               sys/sbuf.h
```

## Important port decisions

- Module parameters retain their compiled defaults.  A future control plane
  should expose a curated native `vfs.zfs.*` sysctl tree.
- `/dev/zfs` uses the full current OpenZFS ioctl namespace.  OpenBSD's ioctl
  envelope has a fixed 32/64-bit layout and points to the standard `zfs_cmd_t`;
  `D_CLONE` device instances preserve per-file-descriptor on-exit and event
  state.  Command authorization remains in the common dispatcher.
- TSD is stored in an SPL table keyed by the OpenZFS key and native
  `struct proc *`; `exit1()` removes entries before process reuse.  OpenBSD's
  non-preemptive kernel model also makes the SPL preemption wrappers and
  filesystem-reclaim transaction marker no-ops.
- ARC defaults to half of physical memory, subject to the common minimum, and
  treats memory above `uvmexp.freetarg` as available.  After native cache
  backoff, the UVM page daemon sends any remaining deficit to ARC without
  waiting for reclamation.  ARC coalesces these notifications and wakes its
  reap thread to reduce the target and start eviction; the common one-second
  pressure poll remains as a fallback.
- ZFS is statically registered as local filesystem type `zfs`, type number 20,
  with a fixed `struct zfs_args` containing the dataset name.  The SPA hostname
  adapter is named `zfs_utsname()` because the kernel already defines an
  `utsname` symbol.
- Authorization uses Unix owner/group/mode bits through `vaccess(9)`.  ZFS ACLs
  are not enforced or exposed; non-local FUID identities map to `nobody`.
  Ownership propagation into external xattr directories is deferred.
- OpenBSD file flags map onto native ZPL immutable, append-only, nodump, and
  opaque attributes.  ZPL has no owner/system distinction for immutable and
  append-only, so both `UF_*` and `SF_*` inputs select the same native bit and
  `stat(2)` exposes that bit in its conservative `SF_*` form.
- Advisory file locks use OpenBSD's native `lockf(9)` manager.  The lock state
  is attached to the in-memory znode and is never stored in the ZFS object.
- OpenBSD `zfs_zget()` returns a referenced, exclusively locked vnode and
  `zrele()` releases it with `vput()`.  Lookup follows native vnode lock order,
  and VFS entry points that depend on the giant lock assert that it is held.
  A short-lived native refcount protects the SA-userdata-to-`vget()` race.
  New vnodes are allocated before DMU transaction assignment and bound to the
  new object afterwards, because `getnewvnode()` may recursively reclaim ZFS.
- Rename discards VFS's asymmetric four-vnode lookup state and re-resolves the
  operation under a per-`zfsvfs` serialization lock.  Directory vnodes are
  locked by object number and directory ancestry is checked before mutation.
- Regular-file I/O currently transfers directly between DMU and `zfs_uio_t`.
  The cache hooks assert that no resident UVM pages exist; mmap, pager support,
  `zfs_rezget()`, and page-backed direct I/O remain deferred.
- ZIL transaction, commit, and vdev flush ordering is unchanged.  ACL replay
  payloads are accepted but ignored by the mode-only authorization layer.
- Native ZFS encryption fails closed with `EOPNOTSUPP`; unencrypted datasets
  retain the common on-disk feature handling.  Channel programs are likewise
  unavailable in the initial port.  Snapshot destruction, which upstream
  implements with an internal channel program, uses an OpenBSD-native atomic
  DSL batch sync task instead.
- Disk vdevs open absolute `/dev` paths with `KERNELPATH`, take capacity and
  logical sector size from the disklabel, and conservatively use at least 4K
  physical ashift.  Ordinary discard is supported; secure discard is not.
- An OpenBSD raw partition (normally `c`) is a whole-device vdev.  Userspace
  keeps the block-device path (for example `/dev/sd2c`) in the pool label and
  deliberately does not create a child partition or write an on-disk
  disklabel.  This supports unpartitioned physical and virtual disks,
  including raw softraid crypto mappings.  Default import discovery scans
  block-device nodes under `/dev` and validates candidates by their ZFS
  labels; raw character-device aliases are not stored because the kernel vdev
  backend requires a block vnode.
- NFS and SMB share management are outside the initial port.  Their libshare
  backends explicitly return `SA_NOT_SUPPORTED`.
- Portable checksum and scalar RAID-Z code is used until OpenBSD provides a
  suitable kernel FPU save/restore contract.
- Userland property discovery treats the statically matched OpenBSD kernel and
  tools as supporting the compiled property tables.  There is no Linux-style
  sysfs property namespace to probe at runtime.
- VFS registration defers the common ZFS runtime through OpenBSD's kernel
  thread deferred queue.  It consequently starts after the scheduler and real
  timecounter, but before mountroot; this lets native taskqs create workers and
  keeps checksum implementation benchmarks off the dummy boot timecounter.
- SPL mutexes and rwlocks remain visible to WITNESS unless OpenZFS constructs
  them with `MUTEX_NOLOCKDEP` or `RW_NOLOCKDEP`.  Those explicit types map to
  `RWL_NOWITNESS`, as their instance-dependent lock graphs cannot be described
  by OpenBSD's static WITNESS classes; DIAGNOSTIC ownership assertions remain
  active for them.  The dnode dbuf-list lock uses this facility because a
  ZPL dnode may precede a metaslab lock while a distinct MOS dnode follows it.
  Main ZAP locks remain fully witnessed, but MOS and dataset ZAPs use separate
  classes: only dataset ZAP growth enters DSL directory space accounting,
  while MOS ZAP updates may already hold a DSL directory lock.
  Znode vnode locks likewise retain ownership checking but not WITNESS order:
  ordinary VOP user-buffer faults and `vnd(4)` backing I/O establish opposite
  shared-lock class edges with process maps.
- Disk strategy submission is deferred through `system_taskq`.  Real disks
  queue strategy requests immediately, whereas `vnd(4)` synchronously enters
  its backing filesystem; the task boundary prevents backing-vnode locks from
  being acquired under arbitrary ZIO caller locks.
- ZFS events remain on the common zevent ioctl queue; there is no `hotplug(4)`
  or FreeBSD `devctl` bridge.
- Pool cachefiles use native vnode I/O.  Removal currently uses the common
  truncate fallback.  Pools containing zvol datasets may be imported, but the
  OpenBSD zvol hooks expose no devices and volume-specific operations return
  `ENOTSUP` until a block-device backend exists.
- The boot-environment nvlist namespace is `openbsd`.  RAID-Z expansion may use
  the boot-reserved area until an OpenBSD ZFS boot loader needs it.
