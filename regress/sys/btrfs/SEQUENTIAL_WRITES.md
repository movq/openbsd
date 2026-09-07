# Sequential-write investigation

Baseline investigated at `bae246e724e`, following the changes recorded in
[PROFILING.md](PROFILING.md). Its remaining bottlenecks are coupled:
sector-sized metadata operations, reservations retained until publication,
quadratic delayed-reference insertion, and frequent full commits. Range
allocation and a reservation model based on affected items are the main
architectural opportunities. Increasing transaction capacity alone exposes
other costs.

## Bounded-range prototype

The initial prototype reached the requested 0.7x FFS2 throughput target, so
performance work paused after indexed references and direct ranges of at most
64 KiB. Its final uninstrumented guest was GENERIC.MP#146, with the same eight
vCPUs, 1 GiB RAM, buffer-cache setting, format and durable 2 GiB workload as below.
All passes used fresh filesystems; builds and other workloads did not overlap
measurement.

| Build / filesystem | Write including unmount, seconds | Generations |
| --- | ---: | ---: |
| FFS2 control before | 3.190 | — |
| Indexed references only, #143 | 8.502 | 1,190 |
| First direct-range prototype, #144 | 3.984 | 192 |
| Final prototype, #146, two passes | 3.959 / 4.136 | 192 / 192 |
| FFS2 control after | 3.419 | — |

The final passes delivered 495–517 MiB/s. Even pairing the slower Btrfs pass
with the faster FFS2 control gives **0.77x FFS2 throughput**; using mean elapsed
times gives approximately 0.82x. This is about
twice the baseline Btrfs throughput. Child system CPU fell to 2.44 / 2.59
seconds. The control variation remains part of this VM measurement.

The retained changes are:

* Separate red-black indexes match metadata and data delayed references by
  complete identity. Work queues put adds/conversions before drops, and data
  delta sign changes move the queue entry. Cancellation and commit coalescing
  update both queue and index. Metadata reference-count queries visit only the
  matching allocation. Indexing alone did not improve this small-transaction
  workload, but removes its quadratic insertion algorithm.
* Each write handle replaces at most one old mapping with a directly allocated
  range of at most `MAXBSIZE`, retaining any old prefix/suffix. The existing
  single-mapping metadata budget covers mapping/split, inode, checksum and
  delayed tree work; inline conversion and explicit holes add their own costs.
  On this layout a 64 KiB range promises 2 MiB of metadata instead of 32 MiB.
  Reservation or contiguous-allocation failure reduces the range before copy.
* Pending lookup finds a containing live range. Overwrites update its payload;
  short truncation and partial-copy failure retain complete allocation payloads
  for checksums while exposing only the live file prefix. Final cancellation
  releases payload, reference and allocation together. A 32 MiB pending-payload
  watermark causes new joins to commit; existing handles finish bounded work.

The final file still has 32,768 extents of 64 KiB, but full range writes install
these directly. Its file tree has **315 leaves at 49.8% occupancy**, compared
with 3,216 at 4.9% in the baseline. `btrfs check` reports 4.95 MiB of filesystem
tree blocks instead of 50.5 MiB. Sector writes retain the existing commit-time
coalescer. No new phase instrumentation was used, so these runs do not
attribute the remaining elapsed time to individual commit phases.

Validation comprised 33 targeted case passes across the prototype builds:
checksum/coalescing/read smoke tests; 4 KiB and 16 KiB range writes, copy faults,
ENOSPC-prefix fallback, reflinks, snapshots, inline conversion, growth, shrink,
holes and Zstd COW; DUP/bitmap/no-free-space-tree and nodatasum checks; and
resets before and after publication. The publication workload now replaces
four sectors in one range write. The new `write-range` recipe checks pending
overlaps, short truncation, sparse regrowth and faults across range boundaries.
Each recipe includes independent unmounted checks and remount verification.
All benchmark passes also passed full checksum and metadata-mirror checks and
complete zero comparison after OpenBSD remount; the final file passed complete
comparison through a Linux read-only mount. FFS2 controls passed `fsck_ffs -fn`.
The full regression suite was not run.

Artifacts use `seq-prototype-*` names under
`/home/mike/obj/btrfs-architecture-profile`; `seq-prototype-final-fs-tree.txt`
records the final topology. Targeted logs are under
`/home/mike/obj/btrfs-prototype-{smoke,targeted,dup,publication}`.
The reproduction runner below was reused. Larger extents with segmented
payloads, chunk-growth changes, background writeback and overlapping
transactions were not implemented. Publication barriers and queue depth remain
unchanged. The guest was left on #146 with the retained prototype, committed
as `a41f6d993d2`.

## Chunk growth and copy follow-up

A requested follow-up evaluated two small changes separately and together,
using the same fresh format, durable 2 GiB workload and uninstrumented runner.
No build or other workload overlapped measurement.

| Change | Kernel | Write including unmount, seconds | Generations | Throughput gain over fresh baseline |
| --- | --- | ---: | ---: | ---: |
| Committed range writer | #146 | 4.130 / 4.180 | 192 / 192 | — |
| Larger data chunks only | #147 | 3.801 / 3.895 | 80 / 80 | 8.0% |
| Remove commit scratch copies only | #148 | 3.864 / 4.011 | 192 / 192 | 5.5% |
| Both changes | #149 | **3.633 / 3.780** | **80 / 80** | **12.1%** |

Gains use mean elapsed times. FFS2 controls before and after this series took
3.155 / 3.384 seconds. Together the changes reach approximately **0.88x FFS2**
using mean elapsed times, or **0.83x** pairing the slower Btrfs run with the
faster FFS2 control. Child system CPU for the combined passes was 2.18 / 2.44
seconds. These are modest samples with visible control variation.

Data growth now targets up to 256 MiB. Above the 32 MiB base, the target uses
at most a tenth of remaining unallocated physical space, accounting for every
mirror and rounding down to stripe alignment. Metadata/system targets and the
fallback that halves a target to fit physical gaps remain unchanged.
The sequential file needed eight new 256 MiB data groups instead of 64 new
32 MiB groups. File extents remain capped at 64 KiB and the pending-payload
watermark remains 32 MiB.

The copy cleanup removes two scratch copies for a full-range data write.
Ordered writeback passes a single payload directly and stages only runs that
combine multiple payloads. Logical submission copies directly from the caller
into each device buffer. Its source must remain stable until return and must
not alias a device buffer that target acquisition/invalidation could recycle.
Both callers satisfy this: ordered payloads are stable after handles drain,
and metadata comes from locked private extent-buffer storage. Async completion
uses the submitted buffer's bytes, never the caller's storage. Checksums,
mirrored submission, error draining, queue depth and publication barriers keep
their existing ordering.

Validation passed 32 selected cases across 4 KiB, 16 KiB, DUP and bitmap
layouts: checksum packing, clustering, coalescing, range writes/reads,
nodatasum, larger data chunks, system growth, device capacity, reclamation,
and recovery before/after publication. Recipes included independent checks,
checksum/mirror validation and remount verification. Every measured Btrfs pass
also passed `btrfs check --readonly --check-data-csum`, metadata-mirror checks
and complete zero comparison after OpenBSD remount. FFS2 controls passed
`fsck_ffs -fn`. The full regression suite was not run.

Two test corrections were needed. The new `chunks-large` recipe uses 8 GiB
devices, except that imported bitmap layouts use 3 GiB to fit the fixture
converter's single-leaf limit; these exercise a scaled growth target.
The capacity test's directory fsync could publish the unlink while leaving
later orphan-cleanup pins unpublished. This also failed on the committed
driver, rebuilt as #150: 11,952 of 12,032 sectors were available until the
later generation published. A full sync before the capacity assertion passed
on that baseline and the combined driver, retaining the requirement that all
data capacity be reclaimed.

Both changes are retained. The guest was returned to GENERIC.MP#151 with the
same driver changes as measured #149; scratch filesystems were left unmounted
in both guests.

Artifacts use `seq-followup-*` under
`/home/mike/obj/btrfs-architecture-profile`. The combined chunk-tree dump is
`seq-followup-both-chunk-tree.txt`; the same reproduction runner below produced
all measurements. Targeted logs are under
`/home/mike/obj/btrfs-followup-{data,large-bitmap,space-final,publication}`.
The original capacity failure and its baseline reproduction/correction are
under `/home/mike/obj/btrfs-followup-{space,capacity-baseline,capacity-baseline-fixed}`.

## Fresh controls and method

The OpenBSD guest ran GENERIC.MP#140, eight vCPUs, 1 GiB RAM,
`kern.bufcachepercent=20`, and virtio-scsi backed by host-cached sparse images.
The workload and format match [BENCHMARKS.md](BENCHMARKS.md): fresh 100 GiB
filesystem, 16 KiB nodes, 4 KiB sectors, SINGLE data, DUP metadata,
free-space tree, NO_HOLES, no block-group tree, `noatime`, and a 2 GiB
`dd bs=1m` write including unmount. No measurement overlapped a kernel build
or another workload.

| Filesystem | Elapsed seconds | Child system CPU seconds |
| --- | ---: | ---: |
| FFS2 control before | 3.058 | 1.72 |
| Btrfs, two fresh passes | 8.450 / 8.571 | 5.91 / 5.92 |
| FFS2 control after | 3.166 | 1.71 |

Btrfs reaches 0.36–0.37x FFS2 throughput here. A 0.7x target corresponds
to 4.37–4.52 seconds, requiring approximately twice the current throughput.
This is a target for this VM workload; host caches and OS writeback behavior
are part of the measurement.

## Where the time goes

Two separate HEAD passes sampled all CPUs at 199 Hz, taking 8.594 / 8.636
seconds. There were 1,343 / 1,310 stacks containing a btrfs function.
Inclusive percentages below overlap and must not be added. Sampling includes
interrupt/completion contexts and measures CPU activity, not sleeping time.

| Path | Percentage of btrfs samples |
| --- | ---: |
| Full transaction commit | 53.4–54.7% |
| Sector mutation, `btrfs_write_file_sector` | 32.5–33.4% |
| Ordered data submission and checksum insertion | 19.1–20.2% |
| Commit-time extent coalescing | 13.9–14.8% |
| Metadata writeback | 10.2–11.3% |
| `pmap_kremove`, across all paths | 15.2–15.3% |
| Delayed data-reference insertion, top frame | 6.1–6.2% |
| CRC32C, top frame | 3.4–4.2% |

A temporary instrumented build (#141) timed commit phases with `nsecuptime`,
counted operations, and printed totals once at writable filesystem teardown.
Its two fresh, unsampled passes took 8.402 / 8.545 seconds. Phase times include
CPU execution, I/O waits and scheduling delays; they are not CPU times.

| Commit phase | Elapsed seconds, two passes |
| --- | ---: |
| Ordered data, checksums, submission and completion | 1.374 / 1.408 |
| Coalesce sector file mappings | 0.974 / 0.990 |
| Materialize references and prepare supporting trees | 0.335 / 0.337 |
| Write and drain metadata | 0.724 / 0.743 |
| First device-sync barrier | 1.894 / 1.909 |
| Build/write superblocks | 0.097 / 0.097 |
| Second device-sync barrier | 0.411 / 0.413 |
| Finish transaction | 0.104 / 0.100 |
| **Total timed commit phases** | **5.913 / 5.997** |
| Time outside these phases | 2.489 / 2.548 |

The barrier measurements cover `btrfs_sync_device`, including device-vnode
fsync and `DIOCCACHESYNC`. They do not isolate the host's flush latency.
Queue-capacity sleeps account for 0.416 / 0.444 seconds and final phase drains
for 0.198 / 0.206 seconds, already included above. Enlarging the existing
16-I/O queue therefore has limited measured headroom. Mapping turnover and
copies still matter, but checksum optimization alone cannot close this gap.

## Work amplification and scaling

Both instrumented baseline passes produced identical operation counts:

* **1,190 commits.** There were 1,061 metadata-reservation failures and 128
  data-reservation failures. The allocator first commits pending work on
  reservation failure; growing a chunk also requires publication.
* **524,288 file-extent inserts, 491,520 deletes, and 32,768 replacements.**
  Commit validates the private sector mappings, enlarges the first mapping
  in each run, then deletes the other fifteen. The final file has exactly
  32,768 extents of 64 KiB each.
* **32,768 physical data writes and 40,218 physical metadata/system writes.**
  Data submission already uses 64 KiB I/Os. Metadata/system traffic totals
  628.4 MiB, including DUP copies, before superblock traffic.

`btrfs_write` reserves 64 metadata blocks per sector. With 16 KiB nodes and
the free-space-tree multiplier, that is **2 MiB of metadata promise per
4 KiB of data**, or 32 MiB per 64 KiB handle. `btrfs_space_keep_delayed`
retains the unused promise until commit. The initial metadata group is
1 GiB, so promises exhaust it after only about 2 MiB of sequential data.
Across all commits, the reserved balance fell by only 0.017% between commit
entry and the end of commit preparation. This measures net consumption,
including any refunds. It is a sum over generations,
not a claim that a terabyte is reserved simultaneously; it also excludes
metadata already allocated by foreground operations. Reservation bounds
must still cover worst-case mutation and delayed work.

Coalescing also leaves the tree sparse. The baseline file tree has 3,216
leaves averaging 10.2 items and **4.9% occupancy**, plus 14 internal nodes.
`btrfs check` reports 50.5 MiB of filesystem-tree blocks. The writer splits
leaves while building sector items, then coalescing deletes most items.
Deletion removes empty leaves but does not merge surviving sparse siblings.
This increases live metadata, cache footprint, and future tree traversal
work as well as the immediate cost of constructing and deleting items.

`btrfs_delayed_data_ref_add` scans a transaction-wide linked list to find
an existing `(bytenr, length, root, inode, file-base)` reference. For N fresh
sector allocations, inserting their distinct references requires
N(N−1)/2 list visits. This is separate from the indexed matching already
used during coalescing.

Two supported format variations tested sensitivity to larger transactions.
These used #141 with CPU sampling; changing format also changes tree work
and I/O, so they are diagnostic comparisons rather than isolated reservation
experiments.

| Layout | Write seconds | Commits | Delayed-ref insertion top-frame samples |
| --- | ---: | ---: | ---: |
| Standard 16 KiB, free-space tree; HEAD sampling | 8.594 / 8.636 | 1,190 | 83 / 80 |
| 16 KiB, no free-space tree | 8.202 | 641 | 153 |
| 4 KiB, free-space tree | 9.841 | 385 | 323 |

The last column corresponds to roughly 0.41, 0.77 and 1.62 sampled CPU
seconds respectively. The growing list cost is consistent with the
quadratic insertion algorithm. Smaller nodes also increased physical
metadata I/Os to 66,630 despite reducing their total bytes. Reducing commit
count alone is insufficient.

## Linux comparison and implementation priorities

Linux source `09f35c36a4ad0` was consulted without importing code.

| Area | Baseline OpenBSD driver | Linux btrfs |
| --- | --- | --- |
| Buffered write | Copy and allocate sectors; install each file mapping immediately | Copy to page cache; reserve and mark delayed-allocation ranges |
| Allocation and completion | Sector-owned pending records; reconstruct 64 KiB extents at commit | Allocate ranges at writeback; ordered completion installs file extents and checksums |
| Extent size | Coalescing capped at `MAXBSIZE` | Normal maximum extent size is 128 MiB, independent of individual I/O limits |
| Reservations | Worst-case sector promise retained until commit | Track outstanding extents and checksum leaves; adjust promises as ranges merge and complete |
| Delayed references | Linear list lookup | Indexed extent heads and per-head reference trees |
| Publication | Writers wait through data, metadata, barriers and superblocks | New transaction can start after root switching while the previous transaction publishes |

Relevant Linux paths are `file.c:btrfs_buffered_write`,
`inode.c:run_delalloc_cow` / `cow_file_range` / `btrfs_finish_one_ordered`,
`delalloc-space.c:btrfs_calculate_inode_block_rsv_size`, `delayed-ref.c`,
and `transaction.c`'s `TRANS_STATE_UNBLOCKED` transition.

For a concrete layout comparison, the Alpine guest (Linux 6.18.48, eight
vCPUs, about 1 GiB RAM) wrote the same 2 GiB file after the identical host
mkfs command on the same scratch disk. It produced **31 extents in one
file-tree leaf and advanced one generation**. The write including unmount
took 2.376 seconds in this single supplemental pass. Different OS
writeback, caching and scheduling prevent treating that time as a prediction
for an OpenBSD implementation. The allocation topology demonstrates that
the on-disk format does not require the present metadata workload.

Recommended sequence:

1. **Index delayed references before increasing transaction capacity.**
   Preserve exact ownership matching, cancellation, and add-before-drop
   ordering. This removes the demonstrated quadratic cost and makes larger
   batches practical. Audit other transaction lists for repeated scans as
   their populations grow.
2. **Make bounded writes allocate and install ranges directly.** Start with
   64 KiB operations and a range-aware pending index. Allocate one extent,
   install one mapping/reference, and encode the inode once. Derive the
   reservation from the affected mappings, splits, tree paths, checksum
   items and delayed work. This removes sector insert/delete churn and its
   sparse-leaf aftermath. It requires coordinated pending overwrite,
   cancellation/splitting, reflink ownership, truncate, partial-copy failure,
   ENOSPC-prefix and abort handling.
3. **Separate extent ownership from payload and physical-I/O granularity.**
   Permit larger allocation ranges backed by bounded payload segments and
   64 KiB submissions. Add explicit dirty-memory and transaction-work limits
   as tighter reservations permit more pending data. The present 32 MiB
   data-chunk growth policy also merits scaling: this file needs 64 growth
   operations, with 128 data-reservation failures/publication retries.
4. **Then consider writeback and transaction overlap.** Full delayed
   allocation and background writeback can overlap allocation, checksums and
   I/O with the writer. Overlapping generations additionally requires root
   versioning and per-generation buffer, payload and pin ownership. The
   measured sector work should be removed before taking on that redesign.

The 0.7x goal needs improvements across these costs. Even removing all
measured coalescing and barrier time would leave about 5.1–5.2 seconds in
the instrumented baseline. Range mutation, fewer metadata I/Os and reduced
CPU work must contribute too. Preserve the publication barriers themselves;
the opportunity is to perform substantially more useful work per commit.

## Reproduction and validation

The host runner is `/home/mike/obj/btrfs-architecture-profile.py`.
Use `seq` for unsampled sequential passes and `seq-profile` for sampling.
`BTRFS_PROFILE_NODESIZE=4096` selects the small-node sensitivity pass;
`BTRFS_PROFILE_FEATURES='^free-space-tree,^block-group-tree,no-holes'`
selects the other variation. The Linux runner is
`/home/mike/obj/btrfs-seq-linux-layout.py`.

Artifacts are in `/home/mike/obj/btrfs-architecture-profile`, with
`seq-investigate-*` prefixes: `head-*` and `ffs-*` controls, `sample-*`
CPU traces, `phases-*` instrumentation runs, `no-fst`, `4k`, and `linux`.
`seq-investigate-instrumentation.patch` records the temporary counters and
timers; `seq-analyze.py` produces `seq-analysis.txt`. The instrumentation
assumes one mounted test filesystem and prints counters in enum order.
Sampling setup and timeout requirements are in [PROFILING.md](PROFILING.md).

All measured btrfs filesystems passed unmounted
`btrfs check --readonly --check-data-csum`, comparison of metadata mirrors,
and complete comparison with zeroes after OpenBSD read-only remount.
FFS2 controls passed `fsck_ffs -fn`. The 4 KiB pass exposed a helper parser
error when the chunk tree acquired an internal node: `mirrors.py` mistook
separator keys for leaf items. Anchoring the item match fixed it; the
rerun checked all 23,066 metadata blocks, followed by full data verification.
The Linux-written filesystem also passed these independent checks.

At the end of the baseline investigation, no driver changes were retained.
The guest was rebuilt and returned to
HEAD-equivalent GENERIC.MP#142, with tracing disabled. The full regression
suite was not run for this profiling investigation.
