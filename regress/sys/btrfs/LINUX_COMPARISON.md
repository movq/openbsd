# OpenBSD / Linux Btrfs workload comparison

The September 18, 2026 comparison identifies two major costs in the OpenBSD
implementation: excessive transaction publication and repeated kernel memory
unmapping. Small persistent extents and large retained reservations produce
hundreds of commits per workload where Linux uses one. Namespace preparation
also allocates and frees whole-node scratch buffers, making TLB shootdowns a
large CPU cost in both extraction and deletion. Repeated individual B-tree
edits add further work. The large-file deletion problem begins in the write
path: OpenBSD deletes a Linux-written 16 GiB file in 0.149 seconds and one
commit.

This investigation measures the sources at `fa02d9fc8be` (same kernel sources
as `68f36cd7fe2`), including the existing cleanup batching fix. The main
measurements use OpenBSD GENERIC.MP#5 and Alpine Linux 6.18.52-0-virt.
The reference Linux source is exactly `v6.18.52`.

## Repeated workloads

Seconds are median (minimum–maximum). There are three complete runs per OS
on three freshly formatted scratch disks, with two 16 GiB passes per run.
Thus tree workloads have three samples and sequential workloads have six.
Mutation times include command execution and the following unmount; reads
and grep measure the command only. Formatting, mounting, observations and
verification are excluded.

Commits are **completed filesystem transactions**, measured by the on-disk
superblock generation difference across each measurement, including its
unmount when timed. They are not transaction-handle joins, syscalls, or
individual metadata updates. Varying counts are shown as ranges.

| Workload | OpenBSD Btrfs seconds | Linux Btrfs seconds | OpenBSD commits | Linux commits |
| --- | ---: | ---: | ---: | ---: |
| Extract source tree | 14.800 (14.639–15.256) | 2.088 (1.734–3.927) | 411–412 | 1 |
| Grep after remount | 8.452 (8.409–8.453) | 1.624 (1.616–1.663) | 0 | 0 |
| Immediate grep repeat | 2.403 (2.393–2.404) | 0.532 (0.532–0.535) | 0 | 0 |
| Recursive chown | 1.168 (1.165–1.174) | 0.548 (0.546–0.550) | 12 | 1 |
| Recursive chmod | 1.184 (1.163–1.228) | 0.548 (0.543–0.673) | 12 | 1 |
| Delete source tree | 14.646 (14.626–14.775) | 0.769 (0.763–0.780) | 626 | 1 |
| Write 16 GiB | 26.484 (25.139–35.210) | 4.985 (4.813–10.539) | 621–720 | 1 |
| Read 16 GiB after remount | 12.474 (11.991–12.671) | 2.807 (2.517–2.993) | 0 | 0 |
| Immediate 16 GiB read repeat | 11.926 (11.724–12.259) | 1.758 (1.716–1.897) | 0 | 0 |
| Delete 16 GiB file | 11.175 (11.109–11.494) | 0.099 (0.085–0.116) | 705 | 1 |

The OpenBSD results reproduce the
[earlier Btrfs/FFS2 comparison](BENCHMARKS_20260918.md). Linux Btrfs is about
7.1 times faster at extraction, 19.0 times faster at tree deletion, and
113 times faster at deleting the large file in these runs. The earlier
OpenBSD FFS2 default times were 8.214, 2.833 and 0.250 seconds respectively;
FFS2 async was 5.725, 0.600 and 0.258 seconds. FFS2 was not rerun here.
The Linux results demonstrate that these gaps are not an unavoidable cost
of the Btrfs on-disk format, although cross-OS timings also include different
VFS, VM, utility and block-I/O implementations.

QEMU counters expose substantial extra publication traffic:

| Workload / OS | Explicit device flushes | Cumulative flush seconds | Bytes written, MiB |
| --- | ---: | ---: | ---: |
| Extraction / OpenBSD | 822 | 4.685 | 2,248.023 |
| Extraction / Linux | 1 | 0.561 | 1,716.668 |
| Tree deletion / OpenBSD | 1,252 | 6.768 | 604.828 |
| Tree deletion / Linux | 1 | 0.006 | 0.695 |
| Large-file deletion / OpenBSD | 1,410 | 7.588 | 568.992 |
| Large-file deletion / Linux | 1 | 0.014 | 1.180 |

Each column is independently aggregated. Cumulative flush time is a backend
counter, not a general additive breakdown of workload time; it is particularly
informative here because OpenBSD synchronously waits for its commit barriers.
Linux also uses FUA on its primary superblock write, which is not counted as
a separate explicit flush. One Linux flush does not mean it omitted durable
superblock publication.

## Extent layouts and deletion

The saved filesystem-tree dumps make the write-layout difference concrete:

| On-disk quantity | OpenBSD writer | Linux writer |
| --- | ---: | ---: |
| Source-tree regular extent items | 175,626–175,634 | 47,648 |
| Source-tree inline extent items | 0 | 38,319 |
| 16 GiB file extent items, first pass | 262,208–262,272 | 128 |
| Typical sequential extent length | 64 KiB | 128 MiB |

OpenBSD has over 2,000 times as many mappings for the large file. Some
allocations split at chunk boundaries, accounting for the small departures
from exactly 262,144 extents. For the source tree, inline storage and merging
successive writes let Linux avoid most separate data-extent references.
The regular-file counts and file bytes are identical on both systems.

`btrfs_write_prepare()` in
[`btrfs_data.c`](../../../sys/btrfs/btrfs_data.c) caps an allocation at
`MAXBSIZE`, 64 KiB. The pending payload and mapping are owned by the
transaction; `btrfs_write_extent_apply()` edits the file-extent tree during
the write. Coalescing is also bounded by `MAXBSIZE`. Linux buffers writes,
uses delayed allocation and creates larger mappings during writeback
(`fs/btrfs/inode.c:run_delalloc_cow`, `fs/btrfs/fs.h:BTRFS_MAX_EXTENT_SIZE`).
The 128 MiB limit is an extent limit, not an individual device-request size.

Deletion must remove file mappings, drop extent references, remove checksums
on the last reference, update free-space records and protect retired storage
until publication. OpenBSD's `btrfs_ref_save()` in
[`btrfs_ref.c`](../../../sys/btrfs/btrfs_ref.c) performs that work per physical
extent. A 16 GiB file made of 262,000 extents therefore presents a fundamentally
different amount of metadata work from a file with 128 extents.

To separate layout from implementation, `cross_delete.py` created a tree and
a 16 GiB file with each writer, unmounted and checked the filesystem, and
saved its exact image. Each deleter received a restored copy of that image.
The large file was deleted first, then the tree. These are single diagnostic
samples; the tree has not undergone the main benchmark's chown/chmod sequence.
Both deleters start the deletion sequence with identical bytes for each
writer's filesystem. Tree deletion follows each OS's large-file cleanup,
so the allocator state at that second measurement can differ.

| Writer / object | OpenBSD deletion seconds / commits | Linux deletion seconds / commits |
| --- | ---: | ---: |
| Linux / 16 GiB file | 0.149 / 1 | 0.105 / 1 |
| OpenBSD / 16 GiB file | 11.377 / 705 | 0.752 / 1 |
| Linux / source tree | 9.026 / 245 | 0.951 / 1 |
| OpenBSD / source tree | 14.719 / 626 | 1.019 / 1 |

The large-file experiment demonstrates both effects: OpenBSD can delete a
large file quickly when its mapping is compact, while Linux can handle
OpenBSD's fragmented mapping without hundreds of commits. OpenBSD's source
cleanup still takes nine seconds on the Linux-created tree, so extent layout
alone does not explain the tree gap. All ten checks (two prepared filesystems
and eight deletions) passed, and each deleted path was confirmed absent after
read-only remount.

## Why transactions accumulate

The eight-item cleanup limit is a **handle/reservation bound**. The previous
fix already removed the unconditional full commit after each eight-item
batch. Increasing that limit alone does not address the current bottleneck.

`btrfs_cleanup_inode()` in
[`btrfs_inode.c`](../../../sys/btrfs/btrfs_inode.c) requests
`nodesize * 64 * (max(1, batch) + 1)` bytes of metadata per batch.
`btrfs_space_reserve()` in
[`btrfs_alloc.c`](../../../sys/btrfs/btrfs_alloc.c) doubles that estimate for
the free-space tree. On this layout an eight-item batch reserves **18 MiB**;
a one-item cleanup reserves **4 MiB**. A create requests **4 MiB**, a namespace
unlink **5 MiB**, and an ordinary write range **2 MiB**, after that doubling.
These are promises, not the number of bytes actually written.

When a handle generates delayed references, `btrfs_space_keep_delayed()`
transfers its remaining metadata/system reservation to the transaction.
`btrfs_run_delayed_refs()` accepts only the commit handle, so these obligations
are normally materialized only after closing the transaction. Reservations
accumulate even when many operations share already-dirty tree paths.

`btrfs_trans_join()` in
[`btrfs_transaction.c`](../../../sys/btrfs/btrfs_transaction.c) responds to
reservation ENOSPC with pending dirty metadata by committing and retrying
before growing a chunk. Thus a mostly empty 100 GiB filesystem can perform
hundreds of commits because allocated metadata space is promised to pending
work, rather than because the device is full.

Linux releases unused transaction reservations at handle end
(`fs/btrfs/transaction.c:btrfs_trans_release_metadata`). Its separate delayed
reference reservation follows actual outstanding reference and checksum
work, with explicit updates, refills and releases
(`fs/btrfs/delayed-ref.c`). Dirty metadata and delayed references can be
processed without making every such flush a full filesystem commit.
Its default periodic commit interval is 30 seconds; pressure and explicit
sync/unmount can force earlier publication. These measured mutations all
finish with one transaction, including unmount.

Writes have another OpenBSD limit: `BTRFS_ORDERED_BYTES_MAX` is **32 MiB**.
Crossing it makes the next join commit the transaction. The 16 GiB sequential
write therefore requires at least roughly 512 payload publications even
before reservation pressure and chunk growth add commits. This limit does
not explain deletion of already-persisted data. The diagnostic counters below
show that reservation pressure currently triggers first: no measured
diagnostic commit was requested by the watermark. Raising only that watermark
would therefore not fix these observed workloads.

## Measured commit causes and CPU costs

After the main and restored-image experiments, a temporary GENERIC.MP#6
added counters and wall-clock phase timers in `btrfs_transaction.c`.
Separate runs sampled all CPUs at 199 Hz using btrace. No timing in the main
table uses that kernel. The tree diagnostic extracts and immediately deletes;
the file diagnostic uses a fresh filesystem without the source tree. These
different initial states explain why file deletion has 620 commits here
versus 705 with the populated tree present.

| Diagnostic workload | Commits | Join commits for metadata reservation failure | Join commits for data reservation failure | Chunk-growth calls | Watermark commits |
| --- | ---: | ---: | ---: | ---: | ---: |
| Extraction | 411 | 396 | 7 | 7 | 0 |
| Tree deletion | 626 | 624 | 0 | 0 | 0 |
| Write 16 GiB | 641 | 512 | 64 | 64 | 0 |
| Delete 16 GiB | 620 | 619 | 0 | 0 | 0 |

The join counters identify requests at those specific call sites, rather
than every caller of commit. Chunk growth also publishes its changes; the
final unmount and other commit callers account for the remaining publications.
In particular, **624 of 626 tree-deletion commits** and **619 of 620
large-file deletion commits** follow metadata-reservation failure.

At extraction commit entry, an average **927.6 MiB** was reserved for
publication, while each commit ultimately wrote an average **699.4 KiB**
of logical dirty metadata. Tree deletion averaged **958.6 MiB reserved**
and **448.1 KiB dirty metadata**. This measures a large gap between the
retained worst-case promises and final work, not physical disk exhaustion.
Final dirty bytes alone are not a safe replacement reservation formula:
transient COW, delayed references and failure recovery still need space.

| Diagnostic workload | Elapsed seconds | Metadata preparation | Barriers + superblocks | Other commit phases | Outside timed commit phases |
| --- | ---: | ---: | ---: | ---: | ---: |
| Extraction | 15.366 | 0.590 | 5.070 | 2.450 | 7.256 |
| Tree deletion | 14.618 | 1.240 | 7.100 | 0.620 | 5.658 |
| Delete 16 GiB | 10.223 | 2.120 | 7.040 | 0.370 | 0.693 |

These are instrumented wall-clock times, including I/O waits. Metadata
preparation includes delayed references, checksums and free-space updates.
Other phases include ordered data, coalescing, metadata writes and transaction
finish. The residual includes foreground namespace/tree work, joining/closing
transactions and userspace; it is not a pure CPU-time estimate. The timers
use `getnsecuptime()`, so small phase values have coarse clock resolution.

CPU sampling identifies a particularly actionable cost in the residual:

| Inclusive path | Extraction, % of Btrfs-containing samples | Tree deletion, % of Btrfs-containing samples |
| --- | ---: | ---: |
| `btrfs_name_plan_free` | 32.9% | 44.8% |
| `pmap_kremove` | 49.5% | 48.5% |
| `x2apic_ipi` | 38.7% | 37.5% |
| `btrfs_trans_commit_closed` | 30.6% | 24.1% |
| `btrfs_mutate_item` | 11.1% | 16.1% |

Denominators are 1,849 extraction samples and 1,505 deletion samples.
These are inclusive, overlapping stack counts, not additive shares of
elapsed time. They locate execution and do not account for time asleep
waiting for the device. Large-file deletion is different: 71.8% of its
681 Btrfs samples contain the commit path, and 57.1% contain metadata
preparation.

The namespace allocation path explains the striking unmap samples:
[`btrfs_dir.c:btrfs_name_edit`](../../../sys/btrfs/btrfs_dir.c) allocates
`nodesize` bytes for every edited namespace key, even for a short name or
small inode reference. A normal creation/removal prepares multiple such
keys. `btrfs_name_plan_free()` frees every buffer at the end of the operation.
With 16 KiB nodes these exceed OpenBSD's `MAXALLOCSAVE` of two pages (8 KiB).
[`kern_malloc.c:free`](../../../sys/kern/kern_malloc.c) therefore calls
`km_free()`, leading to kernel mapping removal and cross-CPU TLB shootdowns.
The traces show this exact `name_plan_free -> free -> km_free ->
pmap_kremove -> x2apic_ipi` chain.

Linux uses slab-cached Btrfs path and delayed-node objects
(`fs/btrfs/ctree.c:btrfs_alloc_path`, `fs/btrfs/delayed-inode.c`) and edits
extent buffers. It does not use this per-key OpenBSD namespace scratch-buffer
allocation/free sequence. Reusing those OpenBSD buffers is a concrete
optimization candidate, independent of the more invasive reservation and
transaction changes. The measurements establish the current cost; no pooled
buffer prototype or speedup claim is included here.

## Tree editing and publication

OpenBSD reaps items individually: find the next item, release the search path,
call `btrfs_delete_item()`, and search again for the writable path. It updates
the remaining inode byte count between batches. `btrfs_mutate_item()` and
`btrfs_leaf_check_edit()` in
[`btrfs_tree.c`](../../../sys/btrfs/btrfs_tree.c) validate the leaf's descriptors
and payload bounds for each edit, then move the packed data/descriptors.
The common path already has an in-place packed-leaf edit; it is inaccurate
to describe every mutation as allocating and rebuilding a complete leaf.
Nevertheless repeated searches, validation and movement remain repeated
per-item work.

Linux's `fs/btrfs/inode-item.c:btrfs_truncate_inode_items()` walks backward
through a leaf, collects adjacent deletions, and calls `btrfs_del_items()`
for the group. It releases/rejoins handles when reservations or transaction
state require it. `btrfs_evict_inode()` in `fs/btrfs/inode.c` ends those handles;
ending a handle does not inherently commit the whole filesystem.

Extraction also repeatedly changes inode and directory metadata. OpenBSD
`btrfs_write_inode()` reads and replaces the inode item during each update,
and namespace operations apply their records directly. Linux's
`btrfs_update_inode()` normally queues an update through
`btrfs_delayed_update_inode()`; directory-index operations also use delayed
items (`fs/btrfs/delayed-inode.c`). Updates to the same parent inode can merge.
Its eviction path can cancel pending inode work which deletion makes
unnecessary. With fewer publication boundaries, dirty tree blocks which
become empty can be discarded before ever being written. The tiny final
Linux tree-deletion write volume is consistent with this.

OpenBSD closes joins until ordered data, delayed references, metadata,
device flush, superblock mirrors and the final device flush complete.
`btrfs_trans_commit_closed()` issues the two explicit barriers measured
above. Linux `btrfs_commit_transaction()` unblocks a new running transaction
before waiting for the old transaction's metadata I/O and superblocks.
`fs/btrfs/disk-io.c:write_dev_supers()` uses FUA for the first superblock,
after the preceding device flush. Batching and the ability to overlap work
both matter; removing required barriers is not a substitute for either.

## Other workloads and interpretation

Chown and chmod have much smaller differences, but OpenBSD still publishes
12 transactions where Linux uses one. They do not create hundreds of
thousands of new data extents. Their gap is consistent with reservation/COW
costs and immediate inode-item updates.

Read performance involves a separate implementation difference. For example,
the first 16 GiB read issues **266,796** QEMU read requests on OpenBSD and
**5,501** on Linux. OpenBSD's range I/O is bounded at 64 KiB; Linux's guest
queue allows 4 MiB requests and reports 8 MiB readahead. This is host-cached
virtual storage, so request dispatch, copying, mapping, checksumming and
readahead can dominate physical-media latency.

The warm grep gap is also not a clean filesystem comparison: Alpine uses
GNU grep and OpenBSD uses its own implementation. The earlier OpenBSD FFS2
and Btrfs warm grep results were essentially equal. Linux's page cache and
OpenBSD's buffer/VM paths differ as well. The 16 GiB file exceeds both guests'
RAM, so an immediate repeat does not mean all bytes are read from guest RAM.
OpenBSD still issues some metadata reads during the warm tree grep.

The widest timing variation occurs in writes and Linux extraction. Explicit
backend flush time for the first Linux write was 4.53 seconds; later samples
were 0.13–0.41 seconds. One OpenBSD write spent 17.78 seconds in flushes.
These ranges describe observed runs on cached VM storage, not confidence
intervals or predictions for physical disks. Commit and extent counts are
more stable explanations of the implementation's work than a single timing.

## Implementation priorities

1. Reuse node-sized namespace preparation buffers through a suitable pool or
   cache. Preserve private ownership for concurrent plans. This directly
   targets the measured `btrfs_name_plan_free`/TLB-shootdown cost.
2. Track the actual remaining delayed-reference/checksum/tree obligations
   separately from unused operation worst-case reservations. Permit bounded
   delayed work to run before reservation pressure requires a full commit.
   Preserve commit and reclaim guarantees for ENOSPC and orphan recovery.
3. Decouple persistent extent length from the 64 KiB I/O/staging limit.
   Merge sequential mappings or allocate larger ranges; retain bounded I/O
   buffers. Small-file inline storage and write aggregation also reduce
   extraction and later cleanup work.
4. Batch adjacent leaf deletions and reuse search paths. Coalesce inode and
   directory-index updates, particularly repeated parent-directory changes.
   Validate at appropriate boundaries while preserving corruption checks.
5. Separate dirty-data memory limits from full transaction publication.
   Later, consider overlapping new mutations with previous-transaction I/O
   and using device FUA where the OpenBSD storage path can guarantee it.

These are architectural follow-ups, not a proposed change to the filesystem's
durability guarantees. No performance change is included in this investigation.

## Environment, validation and reproduction

Both guests have eight vCPUs and 8 GiB RAM and use the same four 100 GiB
virtio-scsi backing files, one active guest/filesystem at a time. QEMU uses
writeback caching; the host is Linux on Btrfs and the scratch files have
NOCOW. Host caches were not dropped. OpenBSD uses `pvclock0` and
`kern.bufcachepercent=20`.

The corpus is the exact archive used in the earlier benchmark:
86,196 files, 7,254 directories, no symlinks, 1,544,684,493 file bytes;
SHA256 `01421823fdbd42e7268a711e980563782a87f0e08b53712b7533d1a35ab070fd`.
It was copied to Alpine's local root filesystem and warmed before each run.
No timed extraction reads over NFS.

Formatting on both guests:

```
mkfs.btrfs -f -K -n 16384 -s 4096 -d single -m dup \
    -O free-space-tree,^block-group-tree,no-holes \
    --checksum crc32c DEVICE
```

Both mkfs versions report the same geometry, initial profiles (1 GiB DUP
metadata, 8 MiB SINGLE data, 8 MiB DUP system), and features: extref,
skinny-metadata, no-holes, free-space-tree. Alpine uses btrfs-progs 6.17.1;
OpenBSD uses the branch's 7.1 port; independent checking uses host 7.1.
Compression, snapshots and quotas are not enabled.

OpenBSD mounts with `rw`. Linux mounts with `rw,nodiscard`, suppressing its
automatic asynchronous discard to match OpenBSD's absence of discard work.
Linux retains default relatime; OpenBSD Btrfs does not update regular-file
read atime. Untimed read-workload unmounts can consequently publish additional
Linux transactions; the table reports only each stated measurement interval.
All mutation measurements include their final unmount, so Linux's deferred
cleanup/writeback is included.

Alpine utilities are GNU tar 1.35, grep 3.12 and coreutils 9.11. OpenBSD uses
base-system utilities. `benchmark.py` uses `bs=1048576` on both systems,
equivalent to the original OpenBSD `bs=1m`, and otherwise preserves the
original command sequence. Its new `--observe` option pauses outside the
timed intervals so the host can read the primary superblock generation and
QEMU block counters. The host acknowledges with `continue`.

The main schedule is Linux then OpenBSD on scratch1, then the same pair on
scratch2 and scratch3. This is repeated testing on shared hardware, not a
randomized study. Independent checks and tree dumps run only while unmounted.
No kernel build or other benchmark overlaps a timed workload.

All six main runs and **84 measurements** completed. All six trees passed
byte-for-byte archive verification and ownership/mode/count checks. All
12 large files were read back and compared completely with zeroes. All
12 grep outputs have identical hashes across OSes and repetitions. All
24 unmounted `btrfs check --readonly --check-data-csum` checks passed.
The four diagnostic mutation checks passed as well; btrace reported no
errors. The temporary instrumentation is saved as `instrumentation.patch`.
The kernel sources were restored, clean objects rebuilt, and the VM returned
to its original GENERIC.MP#5 kernel with `kern.allowdt=0`. Both guests were
left with the scratch filesystems unmounted.

The host artifact directory is `/home/mike/obj/btrfs-linux-comparison/`.
It contains `matrix.py`, `cross_delete.py`, `profile.py`, `analyze.py`,
raw JSONL measurements, `timings.csv`, `summary.json`, `layouts.json`,
`verification.json`, full tree dumps, checker logs, environment records,
QEMU configuration and command logs. Guest runners and logs are under
`/root/btrfs-linux-comparison/`; the OpenBSD archive remains under
`/root/btrfs-bench-20260918/`.

The drivers explicitly format the disposable scratch disks and must only be
used with that VM/disk mapping. `matrix.py` reproduces the six full runs;
`cross_delete.py` reproduces the restored-image deletion comparison.
