# Btrfs write support

This document describes a path from the current read-only implementation to a
transactional writer.  It is a design note, not a claim that the write path is
implemented.

The first writer should batch changes from multiple operations in one
transaction.  It should not commit after each operation.  It is acceptable for
the first version to stop new transaction joins while a commit is in progress.
Overlapping a committing transaction with the next running transaction is a
later optimization.

The log tree is initially out of scope.  `fsync(2)`, `MNT_WAIT` sync, a
synchronous write, and a clean unmount perform or wait for a full transaction
commit.

## Safety invariants

The following rules are non-negotiable:

* The highest valid superblock generation is the commit point.  No pointer
  reachable from it may refer to data or metadata which is not durable.
* Existing metadata blocks are never overwritten.  A metadata block is copied
  before its first modification in a transaction.
* A physical extent freed by a transaction is pinned and cannot be allocated
  again until the transaction's new superblock is durable.
* Space needed to finish metadata COW, delayed references, checksums, and the
  root tree is reserved before an operation changes visible in-memory state.
* A transaction commit waits for all ordered data extents referenced by that
  transaction before writing the superblocks.
* An I/O or consistency failure aborts the transaction and makes the mount
  read-only.  Continuing with partially updated in-memory trees is unsafe.
* All on-disk structures remain little-endian.  Mutable VFS state should be
  host-endian and encoded only at the tree-item boundary.

## Current architecture

Tree metadata and the physical sectors backing file data are read with
`bread()` on the device vnode.  Consequently, physical disk blocks are already
cached by the OpenBSD buffer cache.  Adding a second cache containing
untracked copies of the same bytes would waste memory and create coherency
problems.

Logical mapping and mirror-aware physical reads are centralized in
`btrfs_io.c`.  Metadata header validation and data checksum validation run
inside the shared mirror loop, so a rejected copy is released before another
DUP mirror is tried.  The interface can also return each mirror's error and the
selected mirror.  Synchronous logical writes now submit every required SINGLE
or DUP copy and retain each copy's error; they do not yet have metadata or
ordered-data callers.  Device cache flushes remain part of the future
transaction commit protocol rather than this block submission primitive.

This physical cache is not, by itself, enough for writes:

* A btrfs metadata block is identified by logical bytenr, owner, level, and
  generation.  A `struct buf` is identified by device vnode and physical block.
* DUP metadata has two physical copies but one logical identity.
* A dirty COW block needs transaction ownership, locking, dirty-list linkage,
  and writeback status which are not represented by the current `struct buf`.
* Regular file reads now use sector-sized buffers indexed by `(vnode, file
  offset)`.  `VOP_STRATEGY` fills a logical buffer from inline, hole,
  uncompressed, or compressed extents, using the physical device buffers
  underneath.  This lets reads observe a future delayed-allocation dirty
  buffer before writeback.  The strategy write side and delayed allocation
  are not implemented yet.
* Mounted trees now use persistent roots and take a locked location snapshot
  for each search.  Root-location publication still needs to be tied to
  transaction commit before those locations can change.
* `bn_inode` is now host-endian mutable state decoded at inode-item lookup.
  Dirty-field bits and the last dirty transaction are represented but remain
  clear while the filesystem is read-only; mutation and writeback still need
  to connect them to transaction ownership.

Allocation-tree decoding and validation is isolated in `btrfs_disk.c`.
`btrfs_alloc.c` constructs a mount-owned, per-block-group free-space index by
subtracting every allocated extent from the existing block-group ranges.  The
mount scan rejects overlapping extents and reconciles each block group's used
bytes, as well as their total, with the selected superblock.

The mount also owns an open transaction at the generation after the selected
superblock.  Transaction handles reserve data, metadata, and system space
against specific compatible block groups before mutation.  Aligned first-fit
allocation consumes only a handle's reservations.  Allocations and pinned
frees remain transaction-owned: abort returns new allocations to free space,
while successful commit publication accounts new allocations as committed and
only then releases pinned extents.  Block-group diagnostics check the
free-list and accounting invariants after each transition.  The public mount
gate remains read-only, so no transaction can currently be joined through VFS
operations.

The commit reserve, delayed references, ordered extents, metadata COW, and
on-disk accounting updates are not implemented yet.

## Initial writable format

Write support should start with a deliberately narrow mount policy:

* One device, CRC32C, and existing SINGLE or DUP chunks only.
* No device add/remove, chunk allocation, balance, relocation, scrub repair,
  send/receive, qgroups, or zoned mode.
* No log-tree replay or log-tree creation.
* No data compression on newly written extents.  Existing supported compressed
  extents remain readable and are replaced with uncompressed COW extents when
  modified.
* No NODATACOW writes initially.  Existing NODATACOW files may either be
  rejected for write or use normal COW; in-place data writes should not be
  implemented as a shortcut.
* No free-space tree or block-group tree initially.  Filesystems with those
  compat-ro features remain read-only until both trees can be updated
  transactionally.
* Shared extents must either be handled correctly or cause writable mount to be
  refused.  A filesystem having snapshots makes a "reference count is one"
  shortcut unsafe.

Read-compatible and write-compatible feature masks must remain separate.  A
feature can be safe to parse while still requiring accounting that the writer
does not implement.

Initially, allocate only from existing block groups.  Returning `ENOSPC` when
they are exhausted is safer and much smaller in scope than changing the chunk
and device trees.

## Main in-memory objects

Names below are illustrative.  They are intended to make ownership explicit,
not to freeze the final C API.

### Filesystem roots

The mount owns persistent `struct btrfs_root` objects for the root, chunk,
extent, device, checksum, optional feature, selected filesystem, and discovered
subvolume trees.  They live in a root table keyed by object ID.  A root
currently contains:

* Current logical bytenr, level, and generation.
* Owner/object ID and a pointer back to the mount.
* A lock protecting the current root location.

Tree searches take a root-location snapshot for the committed view.  Updating
a root will change the persistent root object, not a stack-local copy.
Subvolume roots discovered through root items are cached in the same table.
Before mutation, roots still need the transaction generation in which they
were last COWed and linkage on the current transaction's dirty-root list.

The chunk map should similarly be a mount-owned service with a lock and stable
references.  A raw `bm_chunks` pointer copied into a stack root will not remain
safe once chunk-tree changes are supported.

### Metadata extent buffers

Introduce a btrfs metadata wrapper, referred to here as an extent buffer.  It
is keyed by logical bytenr and records:

* Backing `struct buf` objects or an equivalent nodesize allocation.
* Logical bytenr, owner, level, and generation.
* Reference count and a sleepable read/write lock.
* Uptodate, dirty, writeback, I/O-error, and stale states.
* The transaction which owns a dirty COW copy.
* Dirty-list and logical-cache linkage.

The existing device buffer cache should remain the backing store for clean
physical reads where practical.  The extent-buffer layer supplies logical
identity and btrfs-specific state; it is not an independent unbounded byte
cache.

Clean lookup proceeds as follows:

1. Look up the logical block in the extent-buffer cache.
2. Map logical to physical using the current chunk-map reference.
3. Read a mirror through `bread()` if the block is not uptodate.
4. Validate checksum, FSID, bytenr, owner, generation, level, and item layout.
5. Retain the successful logical object and release failed mirror buffers.

Tree-block validation uses the root's explicit view generation rather than
implicitly using `bm_super.generation`.  Read-only roots initialize that view
to the committed generation.  A future running transaction can therefore use
its own generation when it legitimately contains blocks newer than the last
committed superblock.

Metadata dirtying is always done through a transaction-aware
`btrfs_cow_block()` operation.  On the first write in a generation it allocates
a new logical metadata extent, copies the block, changes header bytenr and
generation, updates the parent pointer (or root location), and queues delayed
reference changes.  Later modifications in the same transaction reuse that
dirty block.

### File data cache and ordered extents

Regular file reads use buffers indexed by file logical sector.  The btrfs
`VOP_STRATEGY` read side resolves extent items and fills these logical buffers,
including across holes and compressed extents.  Physical data remains cached
on the device vnode.  The write side will extend this OpenBSD vnode-buffer
integration with additional state:

* Delayed-allocation and metadata reservations are made before dirtying data.
* A writeback request joins a transaction, allocates a COW data extent, and
  creates an ordered extent before submitting device I/O.
* An ordered extent records file range, disk bytenr and length, checksums,
  transaction, pending I/O count, and final error.
* Data-I/O completion wakes waiters but does not itself perform complex tree
  mutation.
* The writeback owner inserts checksum and file-extent items only in the
  transaction associated with the ordered extent.
* Reads first observe dirty file buffers, then ordered extents, then committed
  extent items.  This prevents stale disk data from being returned after a
  buffered write.

Compressed reads do not map one-to-one through `VOP_BMAP`.  Extent readers now
fill caller-provided memory, allowing `VOP_STRATEGY` to populate a logical file
buffer without exposing physical mappings.  Compressed extents are currently
decompressed once for each logical buffer they intersect; caching larger
decompressed clusters is a later optimization.

Dirty data need not join a transaction at the moment of `uiomove()`.  It can
hold an allocator reservation and join during writeback.  Before returning
from `fsync`, all dirty data for that vnode must be converted to ordered
extents, completed, represented in a transaction, and then fully committed.

### Mutable inode state

Inode items are decoded into host-endian `struct btrfs_inode` fields before
being cached in `struct btrfs_node`.  The state includes size, allocated bytes,
mode, owner, times, flags, generation, last dirty transaction, and dirty field
bits.  Vnode inode lookup confines packed `struct btrfs_inode_item` values to
the tree-item decoder.

The in-memory inode is authoritative while the vnode exists.  Updating it and
its inode item must be coordinated so that a transaction cannot commit an
increased size before the corresponding extent and checksum items are ready.
Use `uvm_vnp_setsize()` and invalidate or update cached pages when truncating or
replacing ranges.

## Transaction model

The mount owns one open transaction:

```
struct btrfs_transaction {
        uint64_t generation;
        enum { OPEN, CLOSING, COMMITTING, COMMITTED, ABORTED } state;
        unsigned int writers;
        int error;

        dirty metadata blocks;
        dirty roots and inodes;
        ordered extents;
        delayed references;
        allocated extents;
        pinned freed extents;
        space reservations and accounting deltas;
};
```

A transaction handle represents one filesystem operation and its reservation.
The expected interface is:

```
btrfs_trans_join(mount, reservation, &handle)
btrfs_space_alloc(handle, type, length, alignment, &bytenr)
btrfs_space_pin(handle, bytenr, length)
btrfs_trans_end(handle)
btrfs_trans_abort(handle, error)
btrfs_trans_close(mount, minimum_generation, &transaction)
btrfs_trans_finish(mount, transaction, error)
```

Joining takes a short mount transaction lock, obtains the open transaction,
increments its writer count, and releases the lock.  It does not hold a global
lock for the duration of the operation.  Vnode and extent-buffer locks protect
the actual objects being changed.  `close` elects one committer, changes OPEN
to CLOSING, and waits for handles to leave.  After the future disk commit
publishes or fails, `finish` performs the corresponding allocator transition
and either installs the next open generation or leaves the mount aborted and
read-only.

Ending the last handle does not normally commit.  Commit requests are
coalesced and can come from:

* `fsync(2)`, `MNT_WAIT` sync, unmount, and synchronous I/O.
* The periodic VFS syncer.
* Dirty-metadata, delayed-reference, or reservation high-water marks.
* Memory pressure or explicit administrative sync.

The first implementation can have one transaction generation at a time.  A
committer changes OPEN to CLOSING, prevents new joins, and waits for existing
handles.  New writers sleep until the commit publishes a fresh OPEN
transaction.  This pauses modification during commit I/O, but it still batches
many concurrent operations and is substantially different from serializing
and committing every change.

A later version may create transaction N+1 while N commits.  That requires
careful root versioning, pinned-space ownership, ordered-extent assignment, and
extent-buffer generations, so it should not be hidden in the first writer.

## Locking

A proposed lock order is:

1. Vnode locks in normal VFS/namei order.
2. Transaction handle/reference, without holding the mount transaction mutex.
3. Root lock.
4. Extent-buffer locks from higher tree level to lower tree level.
5. Allocator/block-group lock.
6. Delayed-reference lock.

The mount transaction mutex protects state transitions, handle counts, and
waiters only.  It must not be held across disk I/O or a tree search.

Tree mutation locks a search path top-down.  Splits may lock the needed sibling
in key order.  Code must not recursively modify the extent tree while holding
an arbitrary filesystem-tree path; such updates are represented as delayed
references and processed from a controlled commit context.

Commit must not acquire arbitrary vnode locks after it has closed the
transaction.  A caller such as `fsync` flushes the relevant vnode data before
requesting the commit.  Mount-wide sync flushes dirty vnodes before closing the
transaction.  The transaction itself tracks everything that commit must wait
for without a vnode scan.

## Reservations and allocation

Build a free-space index per block group at writable mount.  Until free-space
tree writing exists, derive it from block-group bounds minus allocated extents
in the extent tree.  Validate that the result agrees with block-group and
superblock accounting.

Maintain separate concepts:

* Free space: reusable by a new reservation.
* Reserved space: promised to an operation but not allocated.
* Allocated space: assigned in the running transaction.
* Pinned space: freed in the running transaction but still reachable from the
  committed superblock.

Reservations should distinguish data and metadata, while handling mixed block
groups.  Metadata reservations must be pessimistic enough for tree splits,
root COW, extent/checksum items, and delayed-reference expansion.  Keep a
small commit reserve which ordinary operations cannot consume, otherwise
ENOSPC can make the transaction impossible to commit.

Allocation updates only in-memory indexes immediately.  Extent items,
backreferences, block-group `used`, device `bytes_used`, and superblock
`bytes_used` are transaction deltas materialized through delayed references.

Delayed references combine repeated add/drop operations for the same extent.
They are essential both for performance and to avoid recursively changing the
extent tree while COWing another tree.  Handle skinny metadata and the active
backreference format explicitly; do not infer them only from item size.

## B-tree mutation

Build mutation below vnode operations.  Required primitives include:

* Search with a transaction handle and a write-locked path.
* COW every block in the path which is not owned by this transaction.
* Insert, replace, and delete leaf items.
* Compact leaf payloads and maintain item offsets.
* Split full leaves and internal nodes, including root growth.
* Update separator keys after the first key in a child changes.
* Remove empty nodes and shrink roots.  More aggressive balancing can wait.
* Mark dirty blocks and roots exactly once per transaction.

Start by testing this engine against synthetic nodes in memory.  Vnode
operations should never open-code item-array movement or parent-pointer
updates.

Root-tree updates need special care.  Dirty subvolume roots update their root
items, and changes to those items can COW the root tree.  The final root-tree
location is written directly into the superblock.  Extent, device, checksum,
and other global roots are likewise represented by root items or superblock
fields according to the on-disk format.

## Commit protocol

The first full commit should have an explicit state machine and fault-injection
points.  At a high level:

1. Flush the data required by the caller into ordered extents.  A mount-wide
   sync does this for all dirty btrfs vnodes before closing the transaction.
2. Serialize with another committer, mark the transaction CLOSING, stop joins,
   and wait for active handles to leave.
3. Wait for all ordered data I/O in the transaction.  If any failed, abort.
4. Insert data checksum and file-extent items and materialize dirty inode
   items.
5. Run delayed references and allocator accounting until no work remains,
   using the commit reserve.  Finalize dirty root items and COW-only roots.
6. Set metadata headers to the transaction generation and compute metadata
   CRC32C checksums after their final modification.
7. Submit every dirty metadata block to every required mirror and wait for all
   required writes.  DUP writes are both required; a degraded-write policy is
   out of scope.
8. Issue `VOP_FSYNC` on the device vnode to drain OpenBSD's dirty device
   buffers, then issue `DIOCCACHESYNC` to force data and metadata through a
   volatile device cache before publishing new roots.  `spec_fsync()` alone
   does not provide the latter guarantee.
9. Construct superblocks with the new generation, roots, levels, accounting,
   backup roots, and system chunk array.  Compute each superblock checksum and
   write all usable mirrors at their fixed physical offsets.
10. Drain the superblock writes and issue a final `DIOCCACHESYNC`.  Only then
    mark the generation committed, publish the committed root locations,
    release pinned extents to free space, and wake waiters.

If failure occurs before any new superblock can be valid, the old filesystem
remains the disk authority, but the in-memory transaction is still aborted.
If a superblock write has been attempted, the durability outcome may be
ambiguous.  The conservative response is to make the mount read-only, return
the error, and require a remount/check rather than attempting to continue.

Writing a superblock with the highest generation is safe only after all blocks
it reaches are durable.  Merely waiting for `bwrite()` completion is not a
substitute for the pre-superblock device cache flush.  Writable mount should
be refused if the backing device cannot supply the required ordering and
durability operation; silently ignoring an unsupported cache-sync ioctl is not
an acceptable policy for btrfs commits.

## VFS operation order

Implement user-visible operations only after the transaction, allocator, and
B-tree mutation layers can be tested directly.  A useful progression is:

1. Update an existing inode item without changing file data, exercised by
   chmod/chown/time updates on a disposable image.
2. Buffered uncompressed writes which replace or append whole sectors,
   including checksum and extent items.
3. Partial-sector writes, holes, truncate, and range replacement.
4. File creation and unlink, with inode allocation, orphan handling, directory
   index/hash items, inode refs, and link-count updates.
5. mkdir/rmdir, rename, hard links, symlinks, and special nodes.

Operations spanning several items use one transaction handle and reserve all
worst-case metadata first.  In-memory namecache changes happen only after tree
mutation has succeeded.  Namespace operations need rollback for errors before
the handle ends; after a fatal transaction error the whole mount is aborted
instead.

Unlinked but open files require orphan items so a crash does not leak extents.
Writable mount recovery of orphan items must exist before unlink is enabled.

`fsync` without a log tree performs:

1. Synchronous writeback of all dirty data for the vnode.
2. Persistence of its inode and extent/checksum updates in the open
   transaction.
3. A full commit of at least that transaction generation.

Concurrent `fsync` calls should wait on and share the same commit rather than
starting redundant commits.

## Preparatory refactors

The following changes are useful before enabling writable mounts:

* Completed: decode vnode inode state to host endian and add
  dirty/transaction fields.
* Completed: refactor regular and compressed extent reads to fill logical file
  buffers, then implement vnode-buffer reads before writes.  Logical buffers
  are one filesystem sector so they align with data checksums and the minimum
  COW unit.
* Completed: split allocation-tree decoders from mutable allocator code and
  construct per-block-group free-space indexes at mount.  The indexes retain
  separate committed-used, free, reserved, transaction-allocated, and pinned
  accounting in preparation for reservations.
* Completed: add the single-open-transaction state machine, typed
  block-group reservations, aligned allocation, transaction-owned allocation
  and pin lists, abort rollback, and commit-publication finalization.  Exact
  data/metadata/system groups are preferred before mixed groups.
* Completed: centralize logical-to-physical read submission.  Metadata and
  data now use one SINGLE/DUP mirror loop, with caller validation participating
  in failover and optional per-mirror error reporting.
* Completed: extend the logical I/O layer with synchronous mirrored write
  submission so metadata/data writes handle SINGLE and DUP consistently and
  report every required-copy failure.  The primitive deliberately does not
  claim stable-media durability; commit must still drain writes and issue the
  required device cache flushes.

These should land in small changes which preserve read-only behavior.  Avoid
adding an ad hoc metadata cache now; the logical extent-buffer API should be
designed together with COW ownership and transaction lifetime.

## Milestones

### 1. Read-path architecture

Completed: host-endian mutable inode state and logical vnode data buffers are
in place.  No writable mount is permitted.

### 2. Transaction and allocator core

Transaction handles, free-space indexes, typed reservations, allocation,
pinning, and commit/abort allocator transitions are in place.  Add a
pessimistic metadata commit reserve, ordered extents, and delayed references.
Exercise the mutation paths with a small in-kernel test harness as they gain
callers, but keep the public filesystem read-only.

### 3. B-tree writer

Implement COW, item mutation, splits, root updates, dirty metadata writeback,
and an internal transaction commit on throwaway images.  Gate it behind a
compile-time diagnostic option until crash tests are credible.

### 4. Existing-file writes

Enable narrowly gated writable mounts and regular uncompressed file writes,
truncate, inode updates, full-commit `fsync`, sync, and unmount.

### 5. Namespace writes

Add create/unlink and orphan recovery, then directories, rename, and links.

### 6. Compatibility and performance

Add free-space-tree and block-group-tree maintenance, more writable feature
bits, asynchronous commit, transaction N+1 overlap, larger clustered I/O,
compression, and eventually the log tree.

## Testing

Every writable test should use a disposable image and validate it with an
independent implementation:

* Run `btrfs check --readonly` after each operation class and remount the image
  with Linux btrfs.
* Compare files, sparse ranges, metadata, checksums, link counts, and free-space
  accounting after remount.
* Force tree height growth and shrinkage with small nodesize images and many
  items.
* Test ENOSPC at data allocation, metadata COW, delayed-reference expansion,
  and commit-reserve boundaries.
* Inject failures before and after data writes, metadata writes, the first
  device flush, each superblock mirror write, and the final flush.
* Crash a VM repeatedly during concurrent create/write/truncate/fsync/sync
  workloads.  After each crash, either the old or new committed state is
  acceptable; dangling references, bad checksums, and mixed generations are
  not.
* Exercise concurrent writers to different files and directories and verify
  that many operations share a transaction generation.
* Keep all existing malformed-image and read-only tests running against the new
  cache and root abstractions.

Useful diagnostic counters include transaction joins and commits, operations
per commit, reserved/allocated/pinned bytes, metadata COWs, delayed-reference
merges, ordered extents, commit latency, and abort reason.

## Decisions to revisit

The following are intentionally deferred:

* Whether extent-buffer bytes live directly in device `struct buf` objects or
  in nodesize memory with device buffers used only during I/O.
* Whether to add a larger decompressed-cluster cache or read-ahead while
  retaining sector-sized vnode buffers.
* Whether the first writable release supports existing snapshots or rejects
  them after a complete root/backreference scan.
* When to allow the next transaction to run concurrently with commit I/O.
* Which free-space-tree representation to emit and when to create new chunks.
* Log-tree replay and log-based `fsync`.

Correctness does not depend on resolving these before the first three
milestones, provided writable mount remains strictly feature-gated.
