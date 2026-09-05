# Btrfs write support

This document records the writer's constraints and design obligations.
Test procedures and coverage belong in `regress/sys/btrfs/README`.

## Supported scope

Writable mounts support regular-file creation and uncompressed sector writes,
directories, inline symlinks, hard links, FIFOs, Unix-domain socket nodes, and
ownership, mode, and timestamp changes. FIFOs use the shared OpenBSD pipe
implementation, including IPC on read-only mounts. Regular files use shared
advisory locking and kqueue facilities. Sync operations commit the full
transaction; there is no log tree.

The writable format is one device, CRC32C, existing SINGLE/DUP chunks, and
skinny metadata. Only `MIXED_BACKREF`, `BIG_METADATA`, `EXTENDED_IREF`,
`SKINNY_METADATA`, and `NO_HOLES` incompat bits are accepted; no compat-ro bits
are supported. Mount requires the newest valid superblock, no pending log,
no seeding device or read-only selected tree, and an extent tree without
legacy extent items, shared references, snapshots, or simple-quota owner refs.
Keep read and write feature masks separate: parsing does not imply maintenance.

Current limits:

* Only the top-level filesystem tree is writable; other subvolumes are readable.
  Simultaneous subvolume mounts require a design discussion before implementation.
  They must share device ownership, allocation, transactions, and caches.
* No truncate, unlink, rmdir, rename, or device-node creation.
* Writes reject inline files, NODATASUM, encoded mappings, and compressed
  overlap. Regular/preallocated uncompressed mappings can be split;
  NODATACOW data is replaced by COW.
* Creation rejects parents with xattrs or NODATACOW pending inheritance support.
  New inodes inherit parent group and compression flags, but write uncompressed
  data. Inline symlink targets are limited to `MAXPATHLEN - 1` bytes.
* Hard links cannot cross trees or target directories. Packed inode references
  overflow into extended references only with `EXTENDED_IREF`; otherwise they
  return `EMLINK`. Hash buckets are limited to one item's capacity.
* Allocation uses existing block groups. There is no chunk allocation,
  free-space-tree/block-group-tree maintenance, device management, relocation,
  log replay, qgroups, or zoned support.

## Transactions and durability

One mount owns one open transaction. Operations join with typed reservations,
encode affected inodes and attach immutable data payloads before ending their
handles. Ending a handle does not commit. A committer closes joins and drains
handles; new writers wait for publication. Commit must not acquire arbitrary
vnode locks.

Reserve worst-case space before visible mutation. Capacity failures must leave
namespace and inode state unchanged and allow later operations. Errors after
partial tree mutation abort the transaction and make the mount read-only.
Mutable inode state is host-endian; encoding preserves unmodeled fields.

The highest valid superblock generation is the commit point:

1. Write ordered data to every required mirror.
2. Drain delayed references, update block-group accounting, and rewrite dirty
   root items to a fixed point. Include allocator change sequence in the
   stability check: this work can COW more trees and queue more references.
3. Finalize metadata headers/checksums and write every required mirror.
4. Drain device buffers and issue `DIOCCACHESYNC`.
5. Write updated superblocks at usable mirrors within the recorded device size.
6. Drain and cache-sync again, then publish in memory and release pinned space.

`bwrite()` and device `VOP_FSYNC` alone do not flush volatile device caches.
Every DUP copy is required. Once superblock writing has been attempted, attempt
the final barrier even if a mirror failed; ambiguous publication requires an
error and read-only mount.

Committed metadata is never overwritten. Abort restores saved roots and marks
transaction-owned extent buffers stale before releasing new allocations.
Detached COW blocks retain allocations until delayed drops remove their extent
items. Freed extents remain pinned until durable publication.

## Cache, locking, and allocation obligations

Device buffers supply physical reads; extent buffers provide logical metadata
identity, validation, locks, and transaction ownership. COW uses private storage
and transaction-aware B-tree APIs. Roots are persistent mount-owned objects:
readers snapshot their locations; writers retain root locks and COW paths from
the root downward. Validate against the path's view generation.

Regular-file buffers use logical sector offsets. Vnode-locked writes modify
temporary sector copies and attach immutable ordered payloads, then update clean
buffers. Cache misses consult ordered data before disk; repeated sector writes
replace the payload and checksum. Strategy writeback is disabled because it
lacks the vnode lock needed for tree/inode mutation.

Lock order is vnode, namespace allocation, transaction handle, root, extent
buffers from top down, allocator/block group, delayed references. The transaction
mutex protects transitions and handles only; never hold it across I/O or tree
searches. Release paths bottom-up and queue reference changes instead of editing
the extent tree recursively. Parent locks protect directory buckets/indexes;
source locks protect link counts/references. Namespace allocation serializes
inode-number selection. Publish name-cache changes and notifications only
after successful mutation.

Free-space indexes subtract allocated extents and physical superblock stripes
from block-group bounds. Stripe exclusions have no extent items and are separate
from block-group usage. Keep free, reserved, allocated, and pinned space distinct,
with typed reservations accounting for mixed groups.

An emergency metadata reserve covers commit and is excluded from ordinary
handles. Failure to establish it rejects writable mount; failure to replenish
after publication leaves that generation durable and the mount read-only.
Operations with delayed work transfer unused reservations to commit. Reservation
failure may commit pending work and retry once before returning `ENOSPC`.

Delayed references merge by extent and ownership. Only the final drop pins an
extent; for data it also removes its checksum range, preserving neighbors.
Hard links change inode references, not data ownership `(root, inode, file-base)`.

## Dependencies for further work

Truncate/range deletion and orphan recovery precede last-link removal.
Writable mount must recover orphans before unlink is exposed. Rename requires
multi-vnode locking and atomic destination replacement.

Chunk allocation must transactionally update chunk/device trees, block groups,
device usage, and possibly the superblock system array. Replace immutable chunk
map pointers with a mount-owned service with safe lifetimes before publication.

Transaction overlap requires root versioning and per-generation ownership of
pinned space, ordered data, and extent buffers. Other later work includes broader
writable formats, clustered I/O, decompression caching, and the log tree.

Validate each operation class with unmounted independent filesystem/data checks
and remount verification. Recovery work also needs reservation exhaustion and
fault injection around data, metadata, cache barriers, and superblock mirrors:
either old or new committed state is valid, mixed generations are not.
