# Btrfs write support

This is a guide to the writer's design constraints and remaining work.
Implementation details belong in the code; test instructions are in
`regress/sys/btrfs/README`.

## Supported operations and format

Writable mounts support regular-file creation and uncompressed sector writes,
mkdir, inline symlinks, hard links, and ownership, mode, and timestamp changes.
Advisory locks and kqueue read/write/vnode filters use OpenBSD's shared VFS
facilities. `fsync`, synchronous I/O, mount sync, and clean unmount perform a
full transaction commit; there is no log tree.

Writable mount validation requires:

* One device, CRC32C, and existing SINGLE or DUP chunks.
* Skinny metadata and only `MIXED_BACKREF`, `BIG_METADATA`, `EXTENDED_IREF`,
  `SKINNY_METADATA`, and `NO_HOLES` incompat bits; no compat-ro bits.
* No pending log root, seeding device, or read-only selected filesystem tree.
* The newest valid superblock generation, without fallback to older mirrors.
* No legacy extent items, simple-quota owner refs, shared block/data refs, or
  snapshots. The complete extent tree is checked before enabling writes.

Additional subvolumes can be traversed, but only the top-level filesystem tree
can be modified. Simultaneous mounts of different subvolumes need a design
discussion before implementation: allocation, transactions, device ownership,
and caches cannot safely be independent for mounts sharing one filesystem.

Current operation limits:

* No truncate, unlink, rmdir, rename, or special-node creation.
* Writes reject inline files, NODATASUM, encoded mappings, and compressed
  overlap. Existing compressed data remains readable. Regular/preallocated
  uncompressed mappings can be split, and NODATACOW data is replaced by COW.
* Creation rejects parents with xattrs or NODATACOW until inheritance is
  implemented. New inodes inherit the parent group and compression flags;
  new data is always uncompressed.
* Symlink targets are inline and limited to `MAXPATHLEN - 1` bytes.
* Hard links cannot cross filesystem trees or target directories. Packed
  per-parent inode references return `EMLINK` when the item fills; extended
  reference insertion remains to be added. Directory hash buckets also have
  a one-item capacity limit.
* Allocation uses existing block groups only. There is no chunk allocation,
  free-space-tree/block-group-tree maintenance, device management, relocation,
  log replay, qgroups, or zoned support.

Read and write feature masks must remain separate: parsing a feature does not
imply that the writer maintains its accounting.

## Durability and failure rules

* The highest valid superblock generation is the commit point. All data and
  metadata reachable from it must already be durable.
* Committed metadata is never overwritten. COW redirects parent pointers or
  persistent root locations and queues delayed extent-reference changes.
* Freed extents remain pinned until the new superblock is durable.
* Reserve worst-case space before visible mutation. Expected capacity failures
  must leave namespace and inode state unchanged and permit later operations.
* An error after partial tree mutation aborts the transaction and makes the
  mount read-only. Do not continue using partially updated in-memory trees.
* Mutable inode state is host-endian; encode little-endian items at the tree
  boundary, preserving fields the writer does not model.

One mount owns one open transaction. Operations join it with typed reservations;
ending a handle does not commit. A committer closes joins and waits for active
handles. New writers wait until publication installs the next generation.
Commit does not acquire arbitrary vnode locks: each operation encodes its inode
items and attaches its data before ending its handle.

Commit order:

1. Close joins and drain operation handles.
2. Write ordered data to every required mirror.
3. Materialize delayed references, update block-group accounting, and rewrite
   dirty root items to a fixed point. These changes can themselves COW trees
   and queue more references; use the allocator's change sequence as well as
   queue emptiness to detect stability.
4. Finalize metadata headers/checksums and write every required mirror.
5. Drain device buffers and issue `DIOCCACHESYNC`.
6. Write superblocks with updated roots, generation, usage, and backup root,
   at usable mirror offsets within the filesystem's recorded device size.
7. Drain and cache-sync again, then publish in memory and release pinned space.

`bwrite()` completion and device `VOP_FSYNC` alone do not guarantee persistence
through a volatile device cache. Every DUP copy is required. Once superblock
writing has been attempted, attempt the final barrier even if a mirror failed;
an ambiguous publication requires an error and read-only mount.

Abort restores saved root locations and marks transaction-owned extent buffers
stale before releasing new allocations. Successful publication releases
transaction ownership only after durability is established.

## Caches, locking, and allocation

The device buffer cache supplies physical reads. The extent-buffer layer adds
logical metadata identity, mirror-independent validation, locks, and transaction
ownership. COW metadata uses private storage so mutation cannot alias committed
device buffers. Tree mutation goes through the transaction-aware B-tree APIs,
which maintain separators, splits, root growth/shrinkage, and delayed implicit
references. Do not bypass them with raw block clones or layout edits.

Roots are mount-owned persistent objects. Readers snapshot their locations
under a lock; write searches retain the root lock and lock/COW paths from the
root downward. Validation uses the path's view generation, which may be newer
than the committed superblock.

Regular-file buffers are indexed by vnode and logical sector. A vnode-locked
writer reads or zero-fills a sector, applies `uiomove` to a temporary copy,
reserves space, and attaches an immutable ordered payload to the transaction.
Successful sectors update clean logical buffers. Cache misses consult ordered
payloads before disk data; repeated writes to a sector replace the earlier
payload and checksum. Strategy writeback is disabled because it lacks the
vnode lock needed for tree/inode mutation. Compressed reads currently repeat
decompression for each logical buffer they intersect.

Lock order:

1. Vnode locks (parent directory before non-directory source for hard links).
2. Namespace allocation lock when allocating new inode numbers.
3. Transaction handle, without retaining the mount transaction mutex.
4. Root lock, then extent-buffer locks from higher to lower levels.
5. Allocator/block-group lock, then delayed-reference lock.

The transaction mutex protects state transitions, handle counts, and waiters;
never hold it across I/O or tree searches. Release paths bottom-up. Queue extent
reference changes instead of recursively editing the extent tree while holding
another tree's path.

Parent locks protect directory indexes and hash buckets. Source vnode locks
protect link counts and packed inode references. The namespace lock serializes
highest-object-ID allocation across creates in different directories. Namespace
items and affected inode items use one handle; update the name cache and emit
notifications only after successful mutation. Directory size is twice the sum
of name lengths, and Btrfs directories have a link count of one.

Free-space indexes are built from block-group bounds minus allocated extents,
with accounting checked against the extent tree and superblock. Keep free,
reserved, transaction-allocated, and pinned space distinct. Reservations are
typed for data/metadata/system space, including mixed block groups.

An emergency metadata reserve is retained for commit and excluded from ordinary
handles. It covers four maximum-height COW/split paths plus accounting margin.
Failure to establish it rejects writable mount; failure to replenish after a
successful commit leaves that generation durable and the mount read-only.
This reserve is not a substitute for budgeting each operation's delayed work.

Delayed references are merged by extent and ownership identity. Only a final
reference drop pins an extent; for data it also removes the physical checksum
range while preserving neighboring checksums. Hard links change inode
references, not data extent references. Data extent ownership is keyed by
`(root, inode, file-base)`.

## Next work and validation

Extend namespace operations together with their recovery requirements:
truncate/range deletion and orphan recovery are prerequisites for removing the
last link of an open file. Writable mount must recover orphan items before
unlink is exposed. Rename requires explicit multi-vnode locking and atomic
handling of destination replacement.

Chunk allocation will need transactional updates to the chunk and device trees,
block groups, device usage, and possibly the superblock system chunk array.
The current immutable chunk-map pointers must become a mount-owned service
with safe lifetimes before publishing map changes.

Later work includes broader writable feature support, clustered I/O and
decompression caching, transaction overlap, and the log tree. Overlap requires
root versioning and ownership of pinned space, ordered data, and extent buffers
across generations.

Use disposable images and run `btrfs check --readonly --check-data-csum` while
unmounted after each operation class, followed by remount verification.
`btrfs restore` can independently verify data without mounting on the host.
Exercise 4 KiB and 16 KiB nodes, tree growth/shrinkage, concurrent writers,
capacity failures, and read-only rejection. Run driver-involving VM commands
under `timeout`; serial/DDB access is needed for hangs.

Recovery testing must also cover allocation/reservation exhaustion and injected
failures around data, metadata, both cache barriers, and superblock mirrors.
Crash workloads may recover either the old or new committed state, but never
dangling references, bad checksums, or mixed generations. Keep malformed-image
and read-only coverage as the writer expands.
