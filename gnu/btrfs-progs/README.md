# btrfs-progs for OpenBSD

This tree contains a minimal OpenBSD port of `mkfs.btrfs` and selected
offline `btrfs` commands from
`btrfs-progs` v7.1. It uses BSD makefiles directly and does not use the
upstream autoconf, automake, or generated build system.

Build it with:

```sh
make
```

The executables are written to `mkfs/mkfs.btrfs` and `cmds/btrfs`.

## Supported

- Creating Btrfs filesystems on regular files and OpenBSD block devices
- Single-device and multi-device filesystems
- Populating a single-device image with basic files, directories, hard links,
  and symbolic links using `--rootdir`
- Standard data and metadata profiles
- CRC32C, xxhash, SHA-256, and BLAKE2 checksums
- Generic file and block-device sizing
- OpenBSD mount and swap-device safety checks
- Offline `btrfs inspect-internal dump-super` for filesystem images and
  devices
- Offline `btrfs inspect-internal dump-tree`, including root summaries,
  selected tree IDs or metadata blocks, traversal modes, hidden names, and
  checksum output
- Offline `btrfs inspect-internal tree-stats` for basic node, level, inline
  data, size, and disk-spread statistics
- Native subvolume list/create/delete and writable/read-only snapshots
- Native online `device add` and `device remove` for SINGLE/DUP filesystems,
  including unequal device sizes and physical relocation of occupied chunks.
  Removal needs unallocated space for each whole chunk on a remaining member;
  balance, profile conversion, and device resizing are not implemented.
- Linux version 1 full and incremental send/receive, including hard links,
  special files, timestamps, and opaque extended attributes. Commands select
  a filesystem by mountpoint and interpret subvolume paths from tree 5;
  see `btrfs(8)` for syntax and limits.

## Not Supported

- Extended attributes and special files in a `--rootdir` source
- Sparse-file preservation; holes are materialized as zero-filled data
- `--rootdir` compression, subvolumes, inode flags, reflink, and shrinking
- Zoned devices and discard/TRIM
- Scanning or registering devices through Linux sysfs or
  `/dev/btrfs-control`
- Operations on mounted Btrfs filesystems through Linux Btrfs ioctls
- Version 2/3 send streams, no-data streams, send clone-source selection,
  and recursive subvolume send
- The ioctl-based `inspect-internal` commands: `inode-resolve`,
  `logical-resolve`, `subvolid-resolve`, `rootid`, `min-dev-size`,
  `list-chunks`, and `map-swapfile`
- Other `btrfs` command groups, including the offline check, rescue, and
  restore tools, are stubbed in this initial port
- A system-wide UUID uniqueness check; an explicit UUID for a multi-device
  filesystem produces a warning

Offline inspection does not scan the host for other devices belonging to a
filesystem. Pass every component device to `dump-tree`; `tree-stats` currently
targets a single-device filesystem.

The copied UAPI headers provide the on-disk structures used by the userspace
filesystem implementation. They do not depend on OpenBSD kernel Btrfs headers.
