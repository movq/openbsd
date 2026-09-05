# Btrfs write support

This document tracks the implemented write path and the remaining work needed
to broaden it.  Writable mounts are exposed only for a deliberately narrow
on-disk format and currently support regular-file creation and data writes,
directory creation, and selected inode attribute changes.

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
or DUP copy and retain each copy's error.  Metadata dirty writeback and
transaction-owned ordered data use this primitive.  Device cache flushes
remain part of the transaction commit protocol rather than this block
submission primitive.

`btrfs_extent_buffer.c` provides the logical identity missing from the device
buffer cache.  Clean extent buffers are keyed by logical bytenr and validate
generation, owner, and level before exposing a locked nodesize buffer.  DUP
copies therefore share one logical object while mirror selection remains in
the logical I/O layer.  Extent buffers are reference counted and mount-cached
while referenced.

Transaction-owned extent buffers now use private nodesize storage so metadata
mutation never aliases a clean physical device buffer.  A low-level clone
operation consumes a handle's metadata reservation, allocates a new logical
block, copies the source, updates its bytenr and generation, clears the WRITTEN
flag and checksum, and adds it to the transaction dirty list.  A companion
constructor uses a transaction-owned source header to create a zeroed sibling
or one-level-higher root block for topology growth.  Mutable access requires
both the transaction handle and the extent-buffer write lock.
Commit-side metadata writeback sets WRITTEN, computes and validates the final
CRC32C, and submits all required mirrors.  The transaction retains dirty
buffers until successful publication or abort; abort marks them stale before
their allocated extents return to free space.

`btrfs_cow_block()` now wraps that low-level clone in the B-tree ownership
changes.  A transaction-aware search holds the root lock and extent-buffer
write locks from the root downward.  It COWs each block not already owned by
the transaction, redirects the locked parent pointer or in-memory root
location, and queues signed delayed tree-reference changes.  A later search
in the same transaction reuses the dirty blocks.

Key-based leaf insertion, replacement, and deletion now use that write search.
Each mutation rebuilds the leaf in zeroed scratch storage, preserving packed
little-endian keys and payloads while compacting all item data.  A changed
first key is propagated through the required ancestor separators.  Insertion
performs a byte-balanced two-way split of a full leaf, recursively splits full
internal nodes, and grows the root.  Writable trees consistently use implicit
block references keyed by root: new blocks queue adds, replaced or detached
blocks queue drops, and moves within a tree leave references unchanged.
Full/shared backreferences remain excluded by writable mount validation.
When a large middle item prevents every two-way partition, insertion
uses three leaves: the old prefix, the new item, and the old suffix.  Both new
sibling pointers are inserted into the parent as one batch, including through
recursive internal-node splits or root growth.  Deleting the sole item in a
leaf now removes empty ancestors and shrinks a one-child root.  If the entire
tree becomes empty, its transaction-owned leaf becomes the level-zero root.
Detached COW blocks cancel their transaction allocations and delayed adds
without losing the required drops of the committed blocks they replaced.

The first root COW also saves the old root location on the transaction's
dirty-root queue.  Abort restores it before stale COW buffers and allocations
are discarded; successful finalization clears transaction ownership only
after metadata publication.  The commit-only delayed-reference engine now
materializes skinny metadata extent items and tree/shared block references to
a fixed point.  It pins a replaced metadata block only after its reference
count reaches zero.  Commit preparation also rewrites block-group usage and
dirty root items until delayed references and allocator state stop changing.
The durable commit path writes finalized metadata, forces it through the
device cache, writes all readable superblock mirrors, forces a second cache
barrier, and only then publishes the generation in memory.  Data references
are included in the published transaction.

Regular file reads use sector-sized buffers indexed by `(vnode, file offset)`.
`VOP_STRATEGY` fills a logical buffer from inline, hole, uncompressed, or
compressed extents, using the physical device buffers underneath.  A
vnode-locked `VOP_WRITE` reads or zero-fills one sector, applies `uiomove()` to
a temporary copy, reserves data and pessimistic metadata space, and
immediately attaches the sector to the open transaction.  Successful sectors
are reflected in clean logical buffers; the strategy read side overlays
transaction-owned ordered payloads if such a buffer is reclaimed before
commit.  Strategy writeback remains disabled because it cannot safely mutate
tree and inode state without the vnode lock.

Mounted trees use persistent roots and take a locked location snapshot for
each read search.  A write search retains the root write lock and publishes
its running-transaction location to later in-memory searches once its COW path
is complete.  The committed location is restored on abort; writing dirty root
items and the final root-tree location into a superblock are now part of
commit.  `bn_inode` is host-endian mutable state decoded at inode-item lookup.
A vnode-locked writer preserves unmodeled inode-item bytes, encodes all mutable
fields, advances the inode transid, and replaces the item in its owning
filesystem tree.  `VOP_WRITE` updates size, allocated bytes, mode, mtime, and
ctime as required.  `VOP_SETATTR` supports owner, group, mode, atime, and
mtime changes with VFS permission checks; size and BSD flag changes are
explicitly unsupported.

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
free-list and accounting invariants after each transition.  Writable VFS
operations join this shared open transaction one sector or inode update at a
time, or one complete namespace operation at a time.

`VOP_CREATE` and `VOP_MKDIR` insert the inode item, inode backreference,
directory hash item, and directory index item together with the parent's
size, sequence, mtime, and ctime update.  Hash collisions append records to
one packed hash item.  The parent vnode lock protects its directory entries;
a mount namespace lock serializes highest-object-ID allocation across
different directories.  Object IDs and directory indexes are allocated above
the current greatest key.  Directories have Btrfs's on-disk link count of one
and size of twice the total name lengths; dot entries are synthesized.
Readers validate directory entry transids against their path's transaction
view, allowing lookup and readdir before commit.

Creation inherits the parent group and uses the caller's uid and VFS mode.
Parents with xattrs or NODATACOW reject creation with `EOPNOTSUPP` pending
inheritance support.  New compression flags can be inherited, but new data is
still written uncompressed.  Synchronous mounts and synchronous directories
commit namespace changes before returning.  Unlink, rmdir, rename, hard
links, symlinks, and special-node creation remain unsupported.

A writable transaction also retains an emergency metadata commit reserve
before any ordinary handle can join.  It is sized for four full-height COW
paths splitting at every level, plus accounting margin.  Ordinary handles
cannot consume it.  After transaction close, the elected committer can obtain
a commit-only handle which allocates from that reserve; only one such handle
may be active at a time.  Commit and abort both release its unused portion.  A
successful commit replenishes the reserve before the next transaction is made
available; failure to replenish leaves the completed generation committed but
forces the mount read-only.  Read-only mounts do not retain this unused
writer-only space.

`IO_SYNC` writes, `VOP_FSYNC`, mount-wide sync, and clean unmount close and
fully commit the relevant open transaction.  Other successful writes may
batch in that transaction.  An already aborted transaction is discarded
safely on unmount, while ordinary commit failure prevents a non-forced
unmount.

## Initial writable format

The writable mount policy currently requires:

* One device, CRC32C, and existing SINGLE or DUP chunks only.
* Skinny metadata, with only `MIXED_BACKREF`, `BIG_METADATA`,
  `EXTENDED_IREF`, `SKINNY_METADATA`, and `NO_HOLES` incompat bits accepted.
* No compat-ro feature bits, including free-space-tree and block-group-tree.
* No pending log root, seeding device, or read-only top-level filesystem tree.
* No fallback from a newer valid superblock mirror to an older generation.
* No legacy extent items, simple-quota owner refs, shared block/data refs, or
  snapshots.  Additional subvolumes may be present and traversed, but their
  vnodes reject mutations with `EROFS`.  The complete extent tree is scanned
  before the transaction subsystem is initialized.
* No device add/remove, chunk allocation, balance, relocation, scrub repair,
  send/receive, qgroups, or zoned mode.
* No log-tree replay or log-tree creation.
* No data compression on newly written extents.  Existing compressed extents
  remain readable, but writes overlapping them return `EOPNOTSUPP`.
* NODATACOW mappings are replaced through normal COW rather than in place.
  NODATASUM files, inline files, and encoded mappings reject writes.

Read-compatible and write-compatible feature masks must remain separate.  A
feature can be safe to parse while still requiring accounting that the writer
does not implement.

Allocation uses only existing block groups.  Returning `ENOSPC` when they are
exhausted is safer and much smaller in scope than changing the chunk and
device trees.

## Main in-memory objects

The sections below distinguish the current objects from fields and behavior
which still need to be added.

### Filesystem roots

The mount owns persistent `struct btrfs_root` objects for the root, chunk,
extent, device, checksum, optional feature, selected filesystem, and discovered
subvolume trees.  They live in a root table keyed by object ID.  A root
currently contains:

* Current logical bytenr, level, and generation.
* Owner/object ID and a pointer back to the mount.
* A lock protecting the current root location.
* The transaction which owns a running-transaction location, if any.

Read searches take a locked root-location snapshot.  A transaction-aware
write search keeps the root write-locked until its path is released, and the
first root COW saves the prior location in a transaction-owned dirty-root
record.  Updating a root changes the persistent root object, not a stack-local
copy.  Subvolume roots discovered through root items are cached in the same
table.  Abort restores the saved location.  Commit preparation now rewrites
the location, generation, level, and generation-v2 fields of every dirty
non-superblock root item.  Final root-tree and chunk-tree locations are encoded
directly in each new superblock.

The chunk map should similarly be a mount-owned service with a lock and stable
references.  A raw `bm_chunks` pointer copied into a stack root will not remain
safe once chunk-tree changes are supported.

### Metadata extent buffers

The btrfs metadata wrapper is `struct btrfs_extent_buffer`.  It is keyed by
logical bytenr and currently records:

* A clean backing `struct buf` or private nodesize bytes for a COW block.
* Logical bytenr, owner, level, and generation.
* Reference count and a sleepable read/write lock.
* Loaded state and a retained validation or I/O error.
* Logical-cache linkage.
* Transaction owner, dirty/writeback/written/stale state, and transaction
  dirty-list linkage.

The transaction owns a reference to every dirty extent buffer, so ending the
operation handle cannot discard modified metadata.  Successful transaction
publication converts written private buffers to clean cached buffers.  Abort
marks them stale and drops transaction ownership before allocator rollback.
Subsequent cache lookup observes the retained error, while private bytes held
by an existing caller cannot alias a physical block which is later reused.

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

Metadata dirtying must always be done through a transaction-aware
`btrfs_cow_block()` operation.  On the first write in a generation it allocates
a new logical metadata extent, copies the block, changes header bytenr and
generation, updates the parent pointer (or root location), and queues delayed
reference changes.  Later modifications in the same transaction reuse that
dirty block.  This operation and a top-down transaction-aware search are now
implemented.  Key-based item insert, replace, and delete operations use this
write path, compact leaf payloads, update ancestor separator keys, perform
two-way leaf and recursive internal-node splits, fall back to three leaves for
a large middle insertion, and grow roots.  A lower-level constructor creates
empty transaction-owned metadata blocks from a locked source header for split
siblings and root growth.  It is not a general mutation API.  Callers must use
the tree operations and must not call the lower-level extent-buffer clone or
constructor or open-code leaf and node layout changes.

### File data cache and ordered extents

Regular file reads use buffers indexed by file logical sector.  The btrfs
`VOP_STRATEGY` read side resolves extent items and fills these logical buffers,
including across holes and compressed extents.  Physical data remains cached
on the device vnode.  The vnode write side connects this OpenBSD vnode-buffer
integration directly to the sector writer:

* Data and pessimistic metadata reservations are made before tree mutation.
* The lower writer allocates one sector from a data block group, records its
  file and physical identity, and retains an immutable transaction-owned
  payload until commit.
* Repeated writes to the same file sector in one transaction replace that
  payload and checksum without leaking the earlier allocation.
* File-extent and checksum items are inserted by the vnode-locked operation;
  commit performs ordered I/O and delayed-reference materialization.
* Commit writes every ordered sector to all required mirrors before preparing
  or writing metadata.
* Successful writes update clean logical buffers.  Cache misses first consult
  ordered extents, then committed extent data, preventing stale disk data from
  being returned before commit.

The initial lower writer rejects inline files, compressed overlap, encoded
data, and NODATASUM files.  Existing regular and preallocated uncompressed
mappings can be split on sector boundaries.  Writable mount rejects shared
data and metadata references because shared-leaf reference conversion is not
implemented.

Compressed reads do not map one-to-one through `VOP_BMAP`.  Extent readers now
fill caller-provided memory, allowing `VOP_STRATEGY` to populate a logical file
buffer without exposing physical mappings.  Compressed extents are currently
decompressed once for each logical buffer they intersect; caching larger
decompressed clusters is a later optimization.

The initial VOP deliberately avoids delayed writeback.  Each successful
partial or full-sector `uiomove()` is represented by an ordered extent before
the operation advances to the next sector.  `fsync` then commits at least the
inode's last dirty transaction generation.

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

        emergency commit reservations;
        allocated extents;
        pinned freed extents;
        dirty metadata extent buffers;
        delayed metadata references;
        delayed data references;
        ordered data extents;
        dirty roots with saved committed locations;
};
```

Dirty metadata, dirty roots, delayed metadata and data references, and ordered
data extents have transaction-owned queues.  Inode items are serialized by
their vnode-locked operation before its handle ends, so closed commit does not
need to acquire arbitrary vnode locks.

A normal transaction handle represents one filesystem operation and its
reservation.  The implemented lifecycle and allocator interface is:

```
btrfs_trans_join(mount, reservation, &handle)
btrfs_space_alloc(handle, type, length, alignment, &bytenr)
btrfs_space_pin(handle, bytenr, length)
btrfs_trans_end(handle)
btrfs_trans_abort(handle, error)
btrfs_trans_close(mount, minimum_generation, &transaction)
btrfs_trans_commit_handle(transaction, &handle)
btrfs_trans_commit(mount, minimum_generation, process)
btrfs_trans_finish(mount, transaction, error)
```

Joining takes a short mount transaction lock, obtains the open transaction,
increments its writer count, and releases the lock.  It does not hold a global
lock for the duration of the operation.  Vnode and extent-buffer locks protect
the actual objects being changed.  `close` elects one committer, changes OPEN
to CLOSING, and waits for handles to leave.  The committer can then obtain one
active commit-only handle at a time without incrementing the writer count.
That handle sees only the transaction's emergency metadata reserve.  After
the durable commit publishes or fails, `finish` performs the corresponding
allocator transition and either installs the next open generation or leaves
the mount aborted and read-only.  `btrfs_trans_commit()` now drives that whole
sequence and lets concurrent requests wait for a commit which already covers
their minimum generation.

Ending the last handle does not normally commit.  Commit requests are
coalesced and can come from:

* `fsync(2)`, `MNT_WAIT` sync, unmount, and synchronous I/O.
* The periodic VFS syncer.
* Dirty-metadata, delayed-reference, or reservation high-water marks.
* Memory pressure or explicit administrative sync.

The implemented state machine has one transaction generation at a time.  A
committer changes OPEN to CLOSING, prevents new joins, and waits for existing
handles.  New writers sleep until the future commit path publishes a fresh
OPEN transaction.  This will pause modification during commit I/O, but still
batches many concurrent operations and is substantially different from
serializing and committing every change.

A later version may create transaction N+1 while N commits.  That requires
careful root versioning, pinned-space ownership, ordered-extent assignment, and
extent-buffer generations, so it should not be hidden in the first writer.

## Locking

The lock order is:

1. Vnode locks in normal VFS/namei order.
2. Transaction handle/reference, without holding the mount transaction mutex.
3. Root lock.
4. Extent-buffer locks from higher tree level to lower tree level.
5. Allocator/block-group lock.
6. Delayed-reference lock.

The mount transaction mutex protects state transitions, handle counts, and
waiters only.  It must not be held across disk I/O or a tree search.

The implemented write search retains the root write lock and locks/COWs the
search path top-down.  Path release drops extent-buffer locks bottom-up before
the root lock.  Splits must lock any needed sibling in key order.  Code must
not recursively modify the extent tree while holding an arbitrary
filesystem-tree path; such updates are represented as delayed references and
processed from a controlled commit context.

Commit does not acquire arbitrary vnode locks after it has closed the
transaction.  Inode items are encoded while the operation still owns the
vnode lock and an ordinary transaction handle.  A caller such as `fsync`
flushes the relevant vnode data before requesting the commit.  Mount-wide sync
will flush dirty vnodes before closing the transaction.  The transaction
itself tracks everything that commit must wait for without a vnode scan.

## Reservations and allocation

The mount builds a free-space index per block group from block-group bounds
minus allocated extents in the extent tree.  It validates the result against
block-group and superblock accounting.  This remains the writable source of
free space until free-space-tree writing exists.

Maintain separate concepts:

* Free space: reusable by a new reservation.
* Reserved space: promised to an operation but not allocated.
* Allocated space: assigned in the running transaction.
* Pinned space: freed in the running transaction but still reachable from the
  committed superblock.

Reservations distinguish data, metadata, and system space while handling
mixed block groups.  Metadata reservations must be pessimistic enough for tree
splits, root COW, extent/checksum items, and delayed-reference expansion.

The implemented emergency commit reserve protects 72 nodesize metadata blocks
from ordinary operations.  This covers four maximum-height paths with a COW
and split at every level, plus eight blocks for root and accounting updates.
It is a last-resort pool, not a replacement for transferring each operation's
delayed-reference and checksum reservation to the transaction.  A writable
mount which cannot establish the full reserve must fail; a transaction which
cannot replenish it after publishing a generation cannot admit more writers.

Allocation updates only in-memory indexes immediately.  Extent items and
backreferences are materialized through delayed references.  Commit
preparation rewrites each block-group `used` value from committed usage plus
new allocations minus pinned frees and retains the resulting total for the
new superblock.  Device-item `bytes_used` does not change because the initial
writer neither allocates nor removes chunks.

Delayed metadata references combine signed add/drop operations with the same
extent, parent, owning root, and level identity.  They are essential both for
performance and to avoid recursively changing the extent tree while COWing
another tree.  A commit-only handle drains them to a fixed point because
COWing the extent tree can queue more work.  New skinny metadata extents are
created with an inline reference; additions to existing extents use explicit
tree/shared block-reference items, and drops handle either representation.
Legacy non-skinny metadata and unsupported inline-reference forms fail
explicitly.

A replaced metadata block is not immediately passed to
`btrfs_space_pin()`.  It may still be referenced by a snapshot or shared tree.
The materializer first applies the drop to the on-disk reference count and
pins the extent only if that count becomes zero.

Delayed implicit data references combine changes by physical extent and the
`(root, inode, file-base)` identity.  Splitting an old file mapping into two
retained sides adds one reference, retaining one side leaves the count
unchanged, and replacing the whole mapping drops one.  New sector extents use
an inline data reference with the standard CRC32C-derived hash.  Existing
inline or separate implicit references can have their count adjusted.  A
final drop removes all checksums covering the physical extent, preserves
neighboring checksum prefixes and suffixes, and pins the extent until
publication.

Accounting and dirty-root updates can COW the extent and root trees, creating
more delayed references and changing allocation state.  Commit preparation
therefore repeats delayed-reference materialization, block-group accounting,
and root-item updates until both the delayed-reference queue and an explicit
space-change sequence are stable.  The sequence avoids mistaking equal-sized
allocation and cancellation activity for a fixed point.

## B-tree mutation

Build mutation below vnode operations.  The initial mutation engine now
provides:

* Search with a transaction handle and a write-locked path.
* COW every block in the path which is not owned by this transaction.
* Insert, replace, and delete leaf items.
* Rebuild leaves in zeroed storage to compact payloads and maintain offsets.
* Update separator keys after the first key in a child changes.
* Split full leaves in two by used bytes, fall back to three leaves when a
  large middle item prevents a valid two-way partition, and recursively split
  full internal nodes.
* Grow a full root and queue implicit references for new nodes; existing nodes
  retain their references when they move within the same tree.
* Mark dirty blocks and roots exactly once per transaction.

Empty-node removal and root shrinking are implemented.  More aggressive
occupancy balancing can wait.

`btrfs_search_slot_write()` retains the transaction handle in the path, holds
the root write lock until `btrfs_release_path()`, and returns every populated
extent buffer write-locked and transaction-owned.  COW redirects only a
transaction-owned parent, while root COW records rollback state exactly once.
`btrfs_insert_item()`, `btrfs_replace_item()`, and `btrfs_delete_item()` own
that path lifecycle and accept keys and payloads in packed on-disk encoding.
They return `ENOSPC` when one item cannot fit in an otherwise empty leaf and
`EFBIG` if insertion would exceed the maximum tree height.  A failure after a
split starts aborts the transaction rather than exposing partially linked
topology.  Deletion removes empty paths recursively, COWs a surviving child
before promoting it as a shorter root, and leaves an empty level-zero root
when the final item is removed.  Traversal also rejects an existing empty
non-root child before attempting to inspect its first or last key.

Start by testing this engine against synthetic nodes in memory.  Vnode
operations should never open-code item-array movement or parent-pointer
updates.

Root-tree updates need special care.  Dirty subvolume roots update their root
items, and changes to those items can COW the root tree.  The final root-tree
location is written directly into the superblock.  Extent, device, checksum,
and other global roots are likewise represented by root items or superblock
fields according to the on-disk format.

## Commit protocol

The implemented commit has an explicit state machine.  VFS writeback
integration and fault-injection points remain to be added.  At a high level:

1. The future data-write caller converts dirty vnode buffers into the
   implemented ordered extents.  A mount-wide sync will do this for all dirty
   btrfs vnodes before closing the transaction.
2. Serialize with another committer, mark the transaction CLOSING, stop joins,
   and wait for active handles to leave.
3. Write all transaction-owned ordered sectors synchronously to every
   required mirror.  Any failed copy aborts the transaction before metadata
   writeback.
4. Checksum, file-extent, and inode items were inserted while the vnode and
   ordinary handle were still held, before transaction close.
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
If a superblock write has been attempted, the implementation performs the
final drain/cache-sync attempt even when one mirror write failed.  The
durability outcome may be ambiguous, so it makes the mount read-only, returns
the error, and requires a remount/check rather than attempting to continue.

Writing a superblock with the highest generation is safe only after all blocks
it reaches are durable.  Merely waiting for `bwrite()` completion is not a
substitute for the pre-superblock device cache flush.  Writable mount should
be refused if the backing device cannot supply the required ordering and
durability operation; silently ignoring an unsupported cache-sync ioctl is not
an acceptable policy for btrfs commits.

Steps 3 through 10 are connected for transactions containing ordered data.
Superblock construction updates the generation, root/chunk locations and
levels, bytes used, log-root fields, and one rotating backup-root slot.  Each
readable in-range mirror gets its own physical `bytenr` and CRC32C.  Strict
mount-time validation now gates the VFS write paths described above.

## VFS operation order

Implement user-visible operations only after the transaction, allocator, and
B-tree mutation layers can be tested directly.  A useful progression is:

1. Update an existing inode item without changing file data, exercised by
   chmod/chown/time updates on a disposable image.
2. Sector-attached uncompressed writes which replace, append, or extend through
   holes, including checksum and extent items.
3. Truncate and general range deletion.
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

## Completed preparatory refactors

The following write-path foundations are in place:

* Completed: decode vnode inode state to host endian and add
  dirty/transaction fields.
* Completed: refactor regular and compressed extent reads to fill logical file
  buffers, then implement vnode-buffer reads before writes.  Logical buffers
  are one filesystem sector so they align with data checksums and the minimum
  COW unit.
* Completed: add persistent mount-owned roots with locked location snapshots
  and logical, validated, reference-counted metadata extent buffers.
* Completed: split allocation-tree decoders from mutable allocator code and
  construct per-block-group free-space indexes at mount.  The indexes retain
  separate committed-used, free, reserved, transaction-allocated, and pinned
  accounting in preparation for reservations.
* Completed: add the single-open-transaction state machine, typed
  block-group reservations, aligned allocation, transaction-owned allocation
  and pin lists, abort rollback, and commit-publication finalization.  Exact
  data/metadata/system groups are preferred before mixed groups.
* Completed: retain a transaction-owned emergency metadata commit reserve
  outside ordinary handles.  Only the elected committer can consume it, and
  commit/abort cleanup plus next-transaction replenishment preserve allocator
  accounting and fail safely on ENOSPC.
* Completed: centralize logical-to-physical read submission.  Metadata and
  data now use one SINGLE/DUP mirror loop, with caller validation participating
  in failover and optional per-mirror error reporting.
* Completed: extend the logical I/O layer with synchronous mirrored write
  submission so metadata/data writes handle SINGLE and DUP consistently and
  report every required-copy failure.  The primitive deliberately does not
  claim stable-media durability; commit must still drain writes and issue the
  required device cache flushes.
* Completed: add transaction-owned private metadata extent buffers.  The
  low-level clone operation consumes reserved metadata space, initializes the
  new COW identity and generation, gates mutable access by handle ownership,
  and retains dirty blocks on a transaction queue.  Final writeback computes
  and validates metadata checksums before mirrored logical writes; publication
  and abort cleanup release or stale the buffers in allocator-safe order.
* Completed: add top-down transaction-aware tree search and
  `btrfs_cow_block()`.  Mutable paths retain the root and extent-buffer locks,
  redirect only transaction-owned parents, reuse blocks already COWed in the
  generation, track root rollback locations, and queue merged delayed metadata
  reference deltas.  Abort restores roots before allocator rollback, while
  successful finalization rejects unmaterialized refs.
* Completed: add key-based leaf item insert, replace, and delete operations.
  Mutations preflight capacity and separator invariants, rebuild compact leaves
  in zeroed scratch storage, and propagate changed first keys through ancestor
  separators.
* Completed: add insertion-time B-tree topology growth.  Full leaves split in
  two by packed bytes, full internal nodes split recursively, and a full root
  grows one level.  New blank metadata blocks remain transaction-owned, while
  new and replaced blocks are represented through delayed implicit-reference
  changes.  Parent changes, including root growth and shrinkage, retain the
  same implicit root reference for each surviving block.
* Completed: add the three-way leaf-split fallback for a large middle item.
  The new item is isolated between the source leaf's valid prefix and suffix,
  and both new sibling pointers propagate atomically through available,
  splitting, or newly grown parents.
* Completed: remove empty nodes recursively and shrink one-child roots.
  Transaction-owned blocks detached during deletion cancel their allocator
  and dirty-buffer ownership, while delayed references retain the committed
  block drops and cancel references to discarded COW blocks.
* Completed: materialize delayed skinny-metadata references from commit
  context.  The fixed-point runner creates and updates metadata extent items,
  supports inline and separate tree/shared block references, and pins an old
  metadata extent only after its final reference is removed.
* Completed: stabilize commit-time metadata accounting and roots.  The
  commit-only preparation loop rewrites block-group usage, captures total
  bytes used for superblock publication, updates dirty root items, and repeats
  whenever those operations create delayed references or change allocator
  state.
* Completed: serialize dirty host-endian inode state back into an existing
  inode item while preserving unmodeled bytes and advancing its transaction
  generation.
* Completed: publish metadata-only transactions durably.  Commit writes and
  validates every dirty metadata buffer, drains and force-syncs the device
  cache, writes every readable superblock mirror with an updated backup root,
  repeats the durability barrier, and only then commits roots and allocator
  state in memory.
* Completed: add sector-sized ordered uncompressed data writes.  The lower
  writer allocates COW data, updates or inserts physical checksums, preserves
  left and right portions of overlapping regular/preallocated mappings,
  updates inode accounting, and coalesces repeated writes to one file sector.
  Commit submits ordered data before metadata preparation.
* Completed: materialize delayed implicit data references.  The commit runner
  updates inline or separate reference counts, creates new data extent items
  with inline references, removes checksum ranges on a final drop, and pins
  freed data until superblock publication.
* Completed: expose strictly gated writable mounts and vnode-locked,
  sector-attached writes for existing regular files.  The VFS layer also
  serializes safe inode attribute changes and performs full commits for
  synchronous writes, `fsync`, mount sync, and clean unmount.

The next layers should continue using the logical extent-buffer and transaction
allocator APIs rather than adding parallel caches or ad hoc reservations.

## Milestones

### 1. Read-path architecture

Completed: persistent roots, logical metadata extent buffers, host-endian
mutable inode state, and logical vnode data buffers are in place.

### 2. Transaction and allocator core

Transaction handles, free-space indexes, typed reservations, allocation,
pinning, commit/abort allocator transitions, and the emergency metadata commit
reserve are in place.  Delayed metadata-reference ownership and merging are in
place, along with skinny-metadata extent-tree materialization and commit-time
block-group accounting.  Delayed implicit data references, ordered sector
writes, and durable superblock publication are in place.

### 3. B-tree writer

Private COW-block allocation, top-down write-locked search, parent/root-aware
COW, dirty-root rollback, delayed metadata-reference queuing, transaction
dirty tracking, leaf item insert/replace/delete with compaction and
separator-key propagation, two-way leaf and recursive internal-node splitting,
three-way leaf fallback, root growth, and final metadata block submission are
in place, as are empty-node removal, root shrinking, and delayed skinny
metadata-reference materialization.  Commit preparation now stabilizes
allocator accounting and dirty root items, and the transaction path now
publishes finalized metadata through two device cache barriers and mirrored
superblocks.  Ordered data and data-reference materialization are also in
place.

### 4. Existing-file writes

In progress: narrowly gated writable mounts, regular uncompressed partial and
full-sector file writes, inode updates, and full-commit `fsync`, sync, and
unmount are implemented.  Truncate and range deletion remain unsupported.

### 5. Namespace writes

Regular-file creation and mkdir are implemented, including inode allocation,
directory hash collisions, index/backreference insertion, and parent metadata.
Next add unlink and orphan recovery, rmdir, rename, and links.

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

`regress/sys/btrfs/namespace.py` exercises create and mkdir on a disposable
mounted image, with a separate verification phase after unmount/check/remount.
It includes concurrent writers, hash collisions across commits, 255-byte names,
permission failures, group inheritance, negative namecache entries, sparse
writes, and directory accounting.  Both 4 KiB and 16 KiB nodesize runs have
passed with independent `btrfs check --readonly --check-data-csum` validation;
the 4 KiB workload grows the filesystem tree to level 2.  Host-side
`btrfs restore` can independently verify file contents without mounting the
image.

`regress/sys/btrfs/enospc.py` fills metadata with empty files and checks that
reservation failure leaves the failed name absent and parent metadata
unchanged, while permitting sync and clean unmount.  A 128 MiB filesystem on
the 1 TiB test device reached ENOSPC after 13,378 new files and passed the
independent checker and writable remount.  Superblock writes are limited to
mirror locations inside the filesystem's recorded device size, even when the
underlying device is larger.

## Decisions to revisit

The following are intentionally deferred:

* Whether to add a larger decompressed-cluster cache or read-ahead while
  retaining sector-sized vnode buffers.
* When to allow the next transaction to run concurrently with commit I/O.
* Which free-space-tree representation to emit and when to create new chunks.
* Log-tree replay and log-based `fsync`.

Correctness does not depend on resolving these before later milestones,
provided writable mount remains strictly feature-gated.
