# Btrfs write support

This document records the writer's constraints and design obligations.
Test procedures and coverage belong in `regress/sys/btrfs/README`.

## Supported scope

Writable mounts support regular-file creation and uncompressed sector writes,
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
without legacy extent items, shared references, snapshots, or simple-quota owner refs.
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
  Remount updates and subvolume creation/property changes are unsupported.
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
* Creation rejects parents with xattrs pending inheritance support.
  New inodes inherit parent group, compression flags, and NODATACOW; regular
  children of NODATACOW directories also receive NODATASUM. Writes still use
  uncompressed COW data. Inline symlink targets are limited to
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
immutable ancestry, and traversal checks directory entries against it.
Unmount flushes only that view's vnodes; the last view closes the device and
destroys filesystem services. Sync or unmount of any view can commit all views.

Operations join with typed reservations, encode affected inodes and attach
immutable data payloads before ending their
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
and transaction-aware B-tree APIs. Roots are persistent filesystem-owned objects:
readers snapshot their locations; writers retain root locks and COW paths from
the root downward. Validate against the path's view generation.

Live I/O copies physical mappings under a short filesystem mapping lock,
released before device access. Only bootstrap roots use fixed chunk tables.
The mapping lock also protects the block-group pointer index. Transaction
handles, commit ownership, or the chunk-allocation lock protect group pointers;
publication drains handles before replacing indexes and freeing removed groups.
`statfs` holds the mapping lock throughout its group traversal.
Mount-time disk validation runs before mutation begins.

Regular-file buffers use logical sector offsets. Vnode-locked writes modify
temporary sector copies and attach immutable ordered payloads, then update clean
buffers. Cache misses consult ordered data before disk; repeated sector writes
replace the payload and checksum. Strategy writeback is disabled because it
lacks the vnode lock needed for tree/inode mutation.
Adjacent data checksums append to packed items, capped at one quarter of a
metadata node. A transaction checksum lock serializes item read/modify/write
across vnodes and range deletion; it is taken after the handle and before roots.
Commit sorts ordered sectors by allocation address and combines adjacent
payloads into writes of at most `MAXBSIZE`, stopping at chunk boundaries.
Before each mirrored write it evicts overlapping device-sector buffers;
device buffers are keyed by their starting block, not their covered range.
While handles are open, file mappings and pending ownership use separate
sector extents. After ordered writes complete, commit combines adjacent sectors
of the same file into regular extents of at most `MAXBSIZE`, within one chunk.
It validates the private mappings and their allocation adds before replacing
them with one mapping and one delayed reference. Sector allocation-accounting
records retain the same disjoint byte ranges until publication. Later COW and
truncation split the committed mappings using ordinary shared extent references.

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
It starts with 32 MiB data/metadata or 8 MiB system chunks and halves the size
down to 1 MiB when device gaps are smaller. Logical ranges append beyond a
mount-lifetime high-water mark. Low system space triggers system growth first.
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

Delayed references merge by extent and ownership. Only the final drop pins an
extent; for data it also removes its checksum range, preserving neighbors.
Hard links change inode references, not data ownership `(root, inode, file-base)`.

Shrinking COWs a retained partial data sector with a zero tail, removes mappings
through the last extent (including preallocation beyond EOF), and invalidates
vnode buffers and mapped pages. Pending sectors that are removed cancel their
delayed adds, checksums, payloads, and unpublished allocations together.
Small shrinks fit one handle. Larger deletions first persist the target size
and an orphan marker, then commit ordered data and delete in reserved batches,
reducing the batch to one item under space pressure. Each batch re-searches
from rounded target EOF and updates remaining byte accounting. The final batch
removes a linked inode's marker; open unlinked inodes retain theirs for last
close. The vnode stays locked through cleanup. Mount recovery uses the same
engine before exposing any view. Failure after publishing the target leaves
the marker recoverable and makes the filesystem read-only.

Final-link removal persists a zero-link inode and an orphan marker in the same
handle as namespace removal. Last-close cleanup first commits ordered data, then
deletes mappings and xattrs in reserved batches, reducing batch size under space
pressure. Each intermediate commit retains the inode, marker, and remaining
byte accounting; the final batch removes inode and marker together. Mount uses
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
writable formats, larger write reservations, clustered reads, decompression caching,
and the log tree.

Validate each operation class with unmounted independent filesystem/data checks
and remount verification. Recovery work also needs reservation exhaustion and
fault injection around data, metadata, cache barriers, and superblock mirrors:
either old or new committed state is valid, mixed generations are not.
