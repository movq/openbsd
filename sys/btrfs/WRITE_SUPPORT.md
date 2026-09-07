# Btrfs write support

This document records the writer's constraints and design obligations.
Test procedures and coverage belong in `regress/sys/btrfs/README`.

## Supported scope

Writable mounts support regular-file creation and uncompressed range writes,
directories, inline symlinks, hard links, FIFOs, Unix-domain socket and device nodes, and
ownership, mode, and timestamp changes. FIFOs use the shared OpenBSD pipe
implementation, including IPC on read-only mounts. Regular files use shared
advisory locking and kqueue facilities. Sync operations commit the full
transaction; there is no log tree.

Directory reads use persistent directory indexes as seek cookies and resume
with a tree search. Index gaps are allowed; indexes whose next cookie cannot
fit in a signed VFS offset return `EOVERFLOW`.

Local file-handle operations (`getfh`, `fhopen`, `fhstat`) identify an inode by
tree, inode number, and creation generation, and enforce the selected mount's
hierarchy. The VFS handle size limits tree IDs and generations to 32 bits;
larger values return `EOVERFLOW`. Handles require a mounted filesystem ID;
IDs do not survive the last view's teardown. NFS export remains unsupported.

Device nodes use native special-device operations and alias handling, including
`nodev` and securelevel policy. Device I/O also works on read-only mounts.
Size changes to devices, FIFOs, and sockets are no-ops, as on FFS.
Device numbers retain their major/minor values using Linux's on-disk encoding;
driver assignments are OS-specific. Creation rejects minors above 20 bits;
loading rejects majors above OpenBSD's 8-bit range with `EOVERFLOW`.
The disk inode uses `(major << 20) | minor`; send streams use Linux's
separate userspace device encoding.

`stat` and `chflags` map btrfs nodump, immutable, and append flags to
`UF_NODUMP`, `UF_IMMUTABLE`, and `UF_APPEND`. Owners may change these flags,
including clearing immutable/append. Other btrfs inode flags are preserved.
New writable opens of append-only regular files require `O_APPEND`; writes
through existing descriptors must start at EOF, as on FFS.
System flags and opaque directories are unsupported: btrfs has no separate
system immutable/append state to enforce OpenBSD securelevel semantics.

The writable format is one device, CRC32C, SINGLE/DUP chunks, and
skinny metadata. Only `MIXED_BACKREF`, `COMPRESS_ZSTD`, `BIG_METADATA`,
`EXTENDED_IREF`, `SKINNY_METADATA`, and `NO_HOLES` incompat bits are accepted.
The free-space-tree (with VALID set) and block-group-tree compat-ro features
are writable, including extent and bitmap free-space records.
Mount requires the newest valid superblock,
no pending log, no seeding device or read-only selected tree, and an extent tree
without legacy extent items or simple-quota owner refs.
Writable mount recovers zero-link file-tree orphans and linked regular-file
truncate markers before exposing any view. Root-tree orphans and cleanup
markers on other linked inode types still reject writable mount.
Keep read and write feature masks separate: parsing does not imply maintenance.

Current limits:

* `mount_btrfs -s subvolid` selects an existing tree; zero or omission selects
  the top-level tree (5). Simultaneous writable mounts must select disjoint
  hierarchies: equal roots and ancestor/descendant pairs return `EBUSY`.
  Nested subvolumes remain traversable, subject to each tree's read-only flag.
  A read-only view may join a writable filesystem; a filesystem first opened
  read-only cannot gain writable views until all views have been unmounted.
  Remount updates and subvolume property changes are unsupported.
* `/dev/btrfs-control` provides privileged subvolume list/create/delete and
  writable or read-only snapshot ioctls. The `btrfs subvolume` commands select
  a filesystem by mountpoint; all paths start at tree 5, including parents
  outside the selected view. Paths do not follow symlinks or `..`.
  Snapshots copy only the root block, sharing lower metadata and file data.
  Root items mark their flags initialized in the embedded inode, so Linux
  retains their read-only state. Lookup selects the newest root-item key;
  updates and deletion retain imported Linux snapshot key offsets.
  Nested subvolumes appear as empty, immutable boundary directories in a
  snapshot; they are not recursively snapshotted.
  Deletion rejects mounted hierarchies, active vnodes, and nested subvolumes.
  It reserves the complete metadata/reference operation and publishes the
  namespace removal and final reference drops atomically. Large deletions
  can return `ENOSPC` before mutation; bounded deletion and recovery remain
  future work.
* `btrfs send` and `receive` support Linux version 1 full and incremental
  streams. A mountpoint selects the filesystem; subvolume and destination
  paths start at tree 5. Tools use existing views or temporary disjoint mounts.
  Send requires read-only roots and holds pathname/inode inventories in
  userspace memory. A privileged, paginated tree-item ioctl uses open root
  descriptors to pin immutable trees; each call excludes root administration
  and releases all tree buffers before returning. Comparing in both directions
  finds changed/deleted items and skips shared subtrees by block address and
  generation, including across different tree heights. Extent iterators compare
  allocation identities and decoded offsets, including split compressed
  mappings, and read file data only for emitted WRITE commands. Unchanged data
  is omitted or cloned from the parent; holes and preallocation are skipped.
  Version 1 requires zero WRITEs when holes replace retained parent data.
  Namespace inventory still visits all names; there is no content-based
  deduplication or search for clone sources at other inode/offset pairs.
  Receive uses ordinary vnode operations and a privileged descriptor-based
  range-clone ioctl. CLONE shares regular data allocations, including compressed
  slices, and preserves holes; inline source data uses bounded COW copies.
  Opaque Linux xattrs use privileged control operations, including for symlinks.
  Send removes old xattrs before creation/data changes and restores them after
  ownership and mode, avoiding Linux ACL inheritance and capability loss.
  Linux ACLs/security labels are preserved as opaque data and are neither
  enforced nor inherited.
  Xattr changes update inode ctime, sequence, and transaction ID atomically.
  Completion atomically publishes the received UUID, sender transaction ID,
  and read-only root flag. Failed/interrupted receives remain incomplete
  writable trees without a received identity. Finalization requires no active
  vnodes or mounted descendant views; the destination must remain private
  during replay. Version 2/3 commands, no-data streams, recursive subvolumes,
  and inode-flag preservation remain unsupported.
  As on Linux receive, ctime is local; symlink permissions are not transmitted.
* Regular-file `truncate`/`ftruncate` and `O_TRUNC` support shrinking
  uncompressed or Zstd regular mappings, preallocation, and supported inline
  files, unchanged sizes, and sparse growth.
  Whole compressed mappings and inline files can be discarded without decoding;
  retaining part of a compressed regular mapping requires Zstd.
  Growth converts supported inline data and COWs partial data sectors with zero
  tails before exposing the new size. It rejects
  other compressed/encoded overlap and regular mappings beyond the old
  rounded EOF; preallocation remains zero-filled. Fragmented shrinking deletes
  mappings in bounded transactions, with a durable target size and recovery
  marker. Partial EOF COW still requires ordinary data and metadata space.
  Unlink preserves open descriptors and mappings and reclaims
  the inode after its last vnode reference. Rmdir validates empty directories
  and uses the same orphan lifecycle. Removed directory descriptors report
  zero links and EOF; new child lookup and creation fail.
* Writes and growth convert uncompressed or Zstd inline files of at most one
  decoded sector to regular extents. Larger inline files, other compression
  codecs, and encoded mappings remain unsupported. Uncompressed and Zstd
  regular mappings and uncompressed preallocation can be split; retained
  compressed pieces keep their original allocation, decoded size, and offsets.
  NODATACOW data is replaced by COW, preserving
  NODATASUM by omitting data checksums. Compressed reads
  support Zstd only.
* Creation permits parents with any Linux xattrs and leaves those attributes
  on the parent. New inodes inherit parent group, compression flags, and
  NODATACOW; regular children of NODATACOW directories also receive NODATASUM.
  Writes still use uncompressed COW data. Inline symlink targets are limited to
  `MAXPATHLEN - 1` bytes.
* Hard links cannot cross trees or target directories. Packed inode references
  overflow into extended references only with `EXTENDED_IREF`; otherwise they
  return `EMLINK`. Hash buckets are limited to one item's capacity. Unlink
  validates and removes the matching hash record, persistent directory index,
  and ordinary or extended reference in one reserved handle. Parent and target
  vnode locks protect the plan; removing a nonfinal name preserves data ownership.
  Unlink and rmdir can borrow protected metadata space at ordinary exhaustion.
* Rename supports same-tree moves and atomic replacement, including nonempty
  source directories and empty destination directories. It preserves source
  inode identity and open destination descriptors. Subvolume roots cannot be
  renamed or replaced; moving a directory below itself is rejected.
* Allocation grows data, metadata, and system block groups within the recorded
  device size, preserving each existing profile. When physical growth fails,
  empty data/metadata groups can be returned for another allocation type.
  One group per profile and all system groups are retained. There is no
  device resizing or management, relocation,
  log replay, qgroups, or zoned support.

`statfs` reports the logical capacity of allocated block groups, counting DUP
once. Free blocks include reservations but exclude pending allocations, pinned
extents, and superblock stripes. Available blocks count only unreserved space
in data-capable groups; capacity changes as chunks are allocated or returned.
Metadata space and fragmentation can still limit writes.

## Transactions and durability

The filesystem instance owns the device, roots, allocation, caches, and one
open transaction. A separate mount view owns the selected root and VFS mount
policy. Transaction failure makes the filesystem and all its views read-only.
Each inode belongs to one view and one vnode, preserving native VM, IPC, locking,
and unmount behavior. Trees have distinct anonymous device IDs for `stat`, with
on-disk inode numbers; these IDs persist until the last view detaches, not across
filesystem lifetimes. A filesystem-local lock serializes view attachment and
detachment; vnode/transaction paths never take it. Root backreferences define
ancestry, and traversal checks directory entries against it. A copied
subvolume entry without a matching backreference denotes a snapshot boundary.
Unmount flushes only that view's vnodes; the last view closes the device and
destroys filesystem services. Sync or unmount of any view can commit all views.

Operations join with typed reservations, encode affected inodes and attach
owned data payloads before ending their
handles. Ending a handle does not commit. A committer closes joins and drains
handles; new writers wait for publication. Commit must not acquire arbitrary
vnode locks.

Reserve worst-case space before visible mutation. Capacity failures must leave
namespace and inode state unchanged and allow later operations. Errors after
partial tree mutation abort the transaction and make the mount read-only.
Mutable inode state is host-endian; encoding preserves unmodeled fields.

The highest valid superblock generation is the commit point:

1. Submit ordered data and insert its checksums, then wait for every required
   data mirror.
2. Drain delayed references, update block-group accounting, and rewrite dirty
   root items to a fixed point. Include allocator change sequence in the
   stability check: this work can COW more trees and queue more references.
3. Finalize metadata headers/checksums, submit every required mirror, and wait
   for completion.
4. Drain device buffers and issue `DIOCCACHESYNC`.
5. Write updated superblocks at usable mirrors within the recorded device size.
6. Drain and cache-sync again, then publish in memory and release pinned space.

`bwrite()` and device `VOP_FSYNC` alone do not flush volatile device caches.
Every DUP copy is required. Once superblock writing has been attempted, attempt
the final barrier even if a mirror failed; ambiguous publication requires an
error and read-only mount.

Data and metadata phases each queue at most 16 physical writes asynchronously.
Logical-write callers supply storage that remains stable until return and does
not alias a device buffer: target acquisition and invalidation can recycle
device buffers. Ordered payloads, staging storage and locked private metadata
satisfy this contract. Submitted buffers own their copies; completion never
accesses the caller's storage. Completion records errors and short I/O,
releases the buffer, then drops the phase's pending count. Every exit drains
the phase before its completion state or transaction resources can be freed;
failed completion prevents advancing to publication. Superblock writes and
the two cache barriers retain their synchronous ordering.

Committed metadata is never overwritten. Abort restores saved roots and marks
transaction-owned extent buffers stale before releasing new allocations.
Detached COW blocks retain allocations until delayed drops remove their extent
items. Freed extents remain pinned until durable publication.

## Cache, locking, and allocation obligations

Device buffers supply physical reads; extent buffers provide logical metadata
identity, validation, locks, and transaction ownership. COW uses private storage
and transaction-aware B-tree APIs. Roots are persistent filesystem-owned objects:
readers snapshot their locations; writers retain root locks and COW paths from
the root downward. Validate against the path's view generation.

Packed leaf edits move payloads and descriptors within private COW storage
after validating capacity and affected ancestor separators. Leaves with gaps
or inputs that alias the leaf use a scratch rebuild. Both paths compact size
changes and zero unused space. Data and metadata CRC32C use the general-register
instruction on amd64 CPUs with SSE4.2, with a portable fallback.

Idle validated metadata retains private bytes in a bounded 8 MiB LRU; device
buffers are released after loading. An address index finds active and cached
blocks. Hits must match generation, level, and permitted owner, and their
validated child generations must fit the caller's view. Allocator-authorized
reuse can evict an idle old generation, never an active reference. Aborted
blocks are discarded; last-view teardown purges the cache after transactions.
Per-filesystem pools reuse metadata-node storage and `MAXBSIZE` I/O scratch
storage, with idle high-water marks of 64 and 16 objects respectively. These
pools carry no cache identity and are destroyed after transaction/cache teardown.
Inodes use a separate index by tree, inode, and snapshot-boundary identity,
with publication and removal serialized by the node-cache mutex.

File-tree blocks may retain another subvolume's on-disk owner. Cache identity
uses logical address, generation, and level; owner validation allows sharing
only between file trees. New COW blocks have implicit references owned by the
writing root. If an owner leaves a shared block, preserve that block's outgoing
edges as full backreferences. Other writers add their own implicit edges; the
last COW of a full-reference block drops its old outgoing edges. Delayed adds
and full-reference conversions precede final drops. Data backreferences support
both `(root, inode, file-base)` and leaf-address ownership.

Administration pins the mount views and serializes directory ancestry. It
locks a visible parent vnode before the namespace lock, closes transaction
joins, drains existing handles, and commits the source generation. The
administrating thread can then reserve and mutate while other joins wait.
It refreshes parent inode and name caches before reopening joins. Deletion
first converts outgoing references owned by the disappearing tree, then drops
edges recursively only when a block loses its last reference. Deleted root
cache entries remain tombstones until filesystem teardown, and root IDs are
not reused during that lifetime.

Live I/O copies physical mappings under a short filesystem mapping lock,
released before device access. Only bootstrap roots use fixed chunk tables.
The mapping lock also protects the block-group pointer index. Transaction
handles, commit ownership, or the chunk-allocation lock protect group pointers;
publication drains handles before replacing indexes and freeing removed groups.
`statfs` holds the mapping lock throughout its group traversal.
Mount-time disk validation runs before mutation begins.

Regular-file reads, including VM-pager reads, use bounded range reads through
temporary storage of at most `MAXBSIZE`. They consult ordered data before disk,
without maintaining a second cache of sector vnode buffers. Vnode-locked writes
replace at most one mapping per handle with a range of at most `MAXBSIZE`.
Reservations cover the affected mapping, retained prefix/suffix, new mapping,
inode, checksum run and delayed tree work, plus inline conversion and explicit
holes. The metadata allowance is per mapping operation, independent of payload
sector count. Reservation or contiguous-allocation failure halves the range down
to one sector before copying or mutation. Each handle encodes its inode,
including a successful prefix before a later copy fault.

Pending ranges are indexed by tree, inode and starting offset under the
transaction lock. Reads and repeated writes find the containing live range;
replacement modifies its payload without splitting its private allocation.
A short truncate or copy fault may leave a live file prefix shorter than the
allocation. Keep the complete allocation payload for writeback and checksums,
but exclude the trimmed suffix from pending lookup. A final mapping deletion
cancels the delayed add, payload and allocation together. Range keys remain
fixed until cancellation or teardown. At 32 MiB of pending payload, new joins
commit before continuing; already joined handles may finish their bounded work.
The compatibility strategy read path uses the same range reader and marks
buffers noncacheable. Strategy writeback is disabled because it lacks the vnode
lock needed for tree/inode mutation.
Physical data reads use windows of at most `MAXBSIZE`, anchored to the backing
allocation and bounded by its length. Split mappings use the same cache keys
and sizes. Each request validates only its sector, allowing different DUP
mirrors to supply healthy sectors from a damaged window; a failed window read
retries the exact sector. Compressed reads use the same windows before decoding.
Device-buffer users check the returned size because cache keys contain only
the starting block, and invalidate mismatched buffers before copying.
Commit sorts ordered ranges by allocation address and combines adjacent
payloads into writes of at most `MAXBSIZE`, stopping at chunk boundaries and
checksum-policy changes. Single payloads are submitted directly; only runs
combining multiple payloads need staging storage. It computes checksums from the
stable payloads and inserts each run into packed items capped at one quarter
of a metadata node.
Checksum insertion and final-drop deletion belong to commit after handles drain;
canceled pending allocations never acquire checksum items. New checksum ranges
must not overlap existing ranges.
Before each mirrored data write it evicts cached buffers starting in the written
range, including for sector writes reusing part of a freed allocation.
Range writes install one file mapping and allocation reference directly.
For sector writes, commit still combines adjacent sectors of the same file into
regular extents of at most `MAXBSIZE`, within one chunk. It validates their
private mappings and allocation adds before replacing them with one mapping
and one delayed reference. Sector allocation-accounting records retain the
same disjoint byte ranges until publication. Later COW and truncation split
committed mappings using ordinary shared extent references.

Without `NO_HOLES`, writes and growth count missing hole items under the vnode
lock and reserve their insertion cost before joining. Fill gaps in the same
handle after data mutation succeeds. Hole splits retain zero disk fields;
preallocated and regular mappings retain their ownership and offsets.

Lock order is rename, vnode, namespace allocation, transaction handle, root, extent
buffers from top down, allocator/block group, delayed references. The transaction
mutex protects transitions and handles only; never hold it across I/O or tree
searches. Release paths bottom-up and queue reference changes instead of editing
the extent tree recursively. Parent locks protect directory buckets/indexes;
source locks protect link counts/references. Namespace allocation serializes
inode-number selection. Publish name-cache changes and notifications only
after successful mutation.

Rename releases incoming vnode locks before taking the filesystem rename lock.
It acquires the involved vnodes with nonblocking attempts, dropping all locks
before waiting for a contended vnode alone. Direct name revalidation refreshes
children after races. The rename lock stabilizes directory ancestry while
checking for cycles. A private image per affected key handles overlapping hash
buckets and inode references before joining one reserved handle; replacement
uses the same orphan lifecycle as unlink.

Creation preallocates a private vnode before joining a transaction and registers
device aliases after ending its handle. Alias registration may lock unrelated
vnodes whose fsync is draining handles. Publish the inode-cache entry only after
alias adoption has fixed the vnode identity.

Free-space indexes subtract allocated extents and physical superblock stripes
from block-group bounds. Stripe exclusions have no extent items and are separate
from block-group usage. Keep free, reserved, allocated, and pinned space distinct,
with typed reservations accounting for mixed groups.

Writable mount validates free-space records against the complement of
the extent tree before excluding superblock stripes from the in-memory index.
Delayed reference materialization splits or coalesces these records and updates
their per-group counts. Bitmap groups retain their representation; counts
describe contiguous free runs, including runs crossing bitmap boundaries.
Free-space tree COW queues ordinary delayed references;
commit drains these to the same fixed point as block-group and root accounting.
Freed ranges become free in the new on-disk tree while remaining pinned in memory
until publication. Block-group usage belongs to the separate block group tree
when enabled, otherwise to the extent tree.

Reservation failure first publishes pending work, then serializes chunk
growth without retaining a handle. The physical planner avoids existing device
extents and superblock stripes, selecting equal-length disjoint stripes for DUP.
The base target is 32 MiB for data/metadata or 8 MiB for system chunks.
Data growth can target up to 256 MiB, limiting growth above the base to a tenth
of the remaining unallocated physical space, including all mirrors.
The planner halves the target down to 1 MiB when device gaps are smaller.
Chunk size does not change extent size or the pending-payload watermark.
Logical ranges append beyond a mount-lifetime high-water mark.
Low system space triggers system growth first.
One reserved handle updates chunk and device trees, device usage, block-group
records, and optional extent-format free-space records. Chunk-tree COW uses
system space. System growth also appends a bootstrap mapping to the superblock
system array, whose capacity is checked before mutation. The new group remains
private until those records are durable; transaction completion publishes the
mapping and group indexes before establishing the next generation's reserves.
Aborted growth frees the private group and exposes no new allocation space.

If physical growth fails, the allocator excludes an empty group of another
type from new reservations, then atomically deletes its chunk, device extent,
block-group, and optional free-space records and reduces device usage. Groups
with disk usage, reservations, allocations, or pins remain ineligible.
Publication returns the stripes to the physical planner; abort restores group
eligibility. Physical buffers are evicted before reassignment because data and
metadata may use different buffer sizes. Empty-group return preserves one group
of each exact profile as a growth template and retains all system mappings.

An emergency metadata reserve covers commit and is excluded from ordinary
handles. A second reserve covers unlink/rmdir, a minimum truncate/orphan-cleanup
batch, or chunk allocation/removal, with system space for chunk-tree COW.
These operations can borrow this promise when ordinary space is unavailable;
its unused portion follows
delayed references to commit and is replenished at publication.
Larger reclaim plans combine that promise with ordinary metadata space;
failed reservations restore the protected promise. Groups with many imported
bitmap records can still exceed the available deletion reservation.
Failure to establish the reserves rejects writable mount; failure to replenish
after publication leaves that generation durable and the mount read-only.
Operations with delayed work transfer unused reservations to commit. Reservation
failure may commit pending work and retry once before returning `ENOSPC`.
Concurrent operations can claim the next generation's space before that retry.
Large namespace reservations may therefore fail under transient pressure.

Delayed references use separate metadata and data indexes keyed by extent and
complete ownership identity. Work queues keep adds and conversions before drops,
including after a data delta changes sign; cancellation removes both index and
queue entries. Coalescing rekeys the enlarged allocation reference.
Only the final drop pins an
extent; for data it also removes its checksum range, preserving neighbors.
Hard links change inode references, not data ownership `(root, inode, file-base)`.

Range cloning locks both regular-file vnodes without waiting on one while
holding the other. It commits their ordered writes before sharing: pending
payloads may otherwise still change in place or mask a replaced mapping.
Source and destination must be on the same filesystem with matching checksum
policy. Offsets are sector aligned; a partial final sector must end at source
EOF and at or beyond destination EOF. Same-inode overlaps are rejected.
Each reserved handle replaces at most one destination mapping with a source
slice, retaining prefix/suffix owners and adding the destination's file-base
reference to the whole allocation. Compressed slices retain decoded offsets;
preallocation is cloned as holes. No data or checksum copy is required for
regular mappings. Destination vnode buffers are invalidated before unlocking.
Large ranges need bounded metadata reservations, but are not atomic as a whole:
an error can leave a completed prefix and sparse growth to the destination
offset. Growing past an old partial EOF or inline prefix can require data COW.

Shrinking COWs a retained partial data sector with a zero tail, removes mappings
through the last extent (including preallocation beyond EOF), and invalidates
vnode buffers and mapped pages. Pending allocations that are removed cancel their
delayed adds, payloads, and unpublished allocations together.
Small shrinks fit one handle. Larger deletions first persist the target size
and an orphan marker, then commit ordered data and delete in reserved batches,
reducing the batch to one item under space pressure. Each batch re-searches
from rounded target EOF and updates remaining byte accounting. The final batch
removes a linked inode's marker; open unlinked inodes retain theirs for last
close. The vnode stays locked through cleanup. Mount recovery uses the same
engine before exposing any view. Failure after publishing the target leaves
the marker recoverable and makes the filesystem read-only.

Final-link removal records a zero-link inode and an orphan marker in the same
handle as namespace removal. Last-close cleanup first commits if that inode has
pending ordered data, then deletes mappings and xattrs in reserved batches,
reducing batch size under space pressure. Validation counts the remaining tree
items so small or final batches reserve only their work, with a floor at the
minimum cleanup budget. Each intermediate commit retains the inode, marker,
and remaining byte accounting. The final batch removes inode and marker
together and can share a transaction with later namespace operations;
minimum-reserve cleanup commits promptly to replenish protected space. Freed
allocations remain unavailable until publication. Mount uses
the same cleanup engine across all file trees, including outside the selected
view. Inode allocation keeps a mount-lifetime high-water mark to prevent reuse
while deleted vnodes or file handles can still exist.

## Dependencies for further work

Partial EOF COW still needs ordinary data and metadata reservations.
Protected cleanup space permits namespace removal, metadata-only shrinking,
and detached inode cleanup under space pressure.

Allocation still needs existing free space to establish mount-time reserves.
Relocating live extents would allow reuse of groups that remain partly occupied.

Transaction overlap requires root versioning and per-generation ownership of
pinned space, ordered data, and extent buffers. Other later work includes broader
writable formats, larger extents backed by bounded payload segments,
decompression caching, and the log tree.

Validate each operation class with unmounted independent filesystem/data checks
and remount verification. Recovery work also needs reservation exhaustion and
fault injection around data, metadata, cache barriers, and superblock mirrors:
either old or new committed state is valid, mixed generations are not.
