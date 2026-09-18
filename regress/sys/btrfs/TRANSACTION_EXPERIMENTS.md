# Btrfs extent and transaction experiments

These experiments follow the [Linux comparison](LINUX_COMPARISON.md),
committed as `7ea13d3173b`. They change commit-time extent coalescing and
metadata reservation-pressure handling. They retain the two device-cache
barriers in a full commit.

The retained changes reduce median source-tree deletion from 14.646 to
7.090 seconds and from 626 to 24 commits. Deleting the 16 GiB file falls
from 11.175 to 0.150 seconds and from 705 commits to one. Extraction and
large-file writing still have excessive commits and show little timing
improvement.

## Changes

The coalescer now accepts complete private pending allocations of different
sizes, including the write path's 64 KiB allocations. It requires physical
and file-offset adjacency, the same inode and root, one unmaterialized
implicit reference per allocation, exact file mappings, and a single chunk.
It excludes shortened pending mappings. Runs can become one on-disk extent
of up to 128 MiB, while payload buffers and individual I/O requests remain
bounded by `MAXBSIZE`.

This happens only during full commit, after data submission and before
reference materialization. The allocator still holds separate records for
the constituent allocations until publication. Reopening a transaction
after this transformation would make subsequent cancellation or retirement
of the merged allocation unsafe, so the reservation experiment deliberately
requires an empty ordered-data queue.

On metadata reservation failure, eligible transactions close joins, drain
active handles, and materialize deferred references, space accounting, and
root items to a fixed point. They then return unused operation reservations
and reopen the same generation. Dirty buffers, saved roots, allocated
extents, and pinned extents remain transaction-owned; this path does not
write metadata or a superblock, release pins, or acknowledge durability.

The drain keeps the original base commit reservation and restores the full
protected metadata and system reclaim reservations before returning surplus
space to ordinary writers. Its accounting ceiling also allows cancellation
of allocations made by previous commit handles. If the promises cannot be
restored, it takes the ordinary full commit path.

The eligibility checks fall back to full commit after 16 successful drains,
at 32 MiB of transaction allocations, or at 128 MiB of pinned space. These
are thresholds checked before a drain, not hard limits on what one operation
or deferred batch can add. Pending ordered data and chunk operations also
require full commit. A failed reservation retry after draining gets a full
commit attempt before chunk growth or ENOSPC.

## Experimental sequence and method

The original baseline has three complete benchmark runs. The coalescer was
first measured alone in one complete run, including two 16 GiB write/read/
delete passes. A first metadata-drain prototype was then measured in three
complete runs. It required the protected reclaim reservation to remain
intact, rather than replenishing a borrowed promise.

That first drain prototype reduced recursive chown/chmod from 11 to 3
commits, but reduced tree deletion only from 416 to 392–393 commits. The
deletion path borrows the protected reservation under pressure and transfers
its remainder to the commit pool. Refusing to restore that promise forced
publication at almost every subsequent reservation failure. The retained
implementation restores it from unused reservations only after materializing
the work those reservations covered.

Measurements use the same archive, format, benchmark script, and observation
protocol as the original comparison. Mutations include the following
unmount. Completed transactions are superblock generation differences;
device counters come from QEMU. Formatting, checkpoints, content verification,
and mounts are outside the timed intervals. No kernel builds or other
benchmarks run during measurements.

The coalescer alone reduced source-tree regular extent items from about
175,630 to 86,139, and the first 16 GiB file from about 262,240 to 688.
Deleting that file took 0.144–0.155 seconds and two commits. Tree deletion
took 11.209 seconds and 416 commits. Extraction took 15.010 seconds and 408
commits; the write passes took 24.091–26.337 seconds and 581–688 commits.
These are diagnostic samples, not independently repeated medians.

## Retained implementation results

The refined implementation has three complete runs, with two 16 GiB passes
per run: 42 measurements, three samples per tree workload and six per
sequential workload. Each run freshly formats scratch1. Baseline medians and
commit counts come from the original comparison's three runs on three
scratch disks. These are sequential VM experiments, not randomized paired
trials; ranges describe observed samples.

| Workload | Baseline seconds | Experiment seconds, median (range) | Baseline commits | Experiment commits |
| --- | ---: | ---: | ---: | ---: |
| Extract source tree | 14.800 | 14.568 (14.511–14.663) | 411–412 | 404–405 |
| Grep after remount | 8.452 | 7.010 (6.969–7.034) | 0 | 0 |
| Immediate grep repeat | 2.403 | 2.400 (2.389–2.408) | 0 | 0 |
| Recursive chown | 1.168 | 1.162 (1.143–1.169) | 12 | 3 |
| Recursive chmod | 1.184 | 1.192 (1.168–1.207) | 12 | 3 |
| Delete source tree | 14.646 | 7.090 (7.011–7.224) | 626 | 24 |
| Write 16 GiB | 26.484 | 25.414 (24.150–26.750) | 621–720 | 581–689 |
| Read 16 GiB after remount | 12.474 | 12.555 (12.269–12.767) | 0 | 0 |
| Immediate 16 GiB read repeat | 11.926 | 11.940 (11.677–12.138) | 0 | 0 |
| Delete 16 GiB file | 11.175 | 0.150 (0.141–0.155) | 705 | 1 |

Tree deletion is about 2.1 times faster, and large-file deletion about 74
times faster. Chown/chmod publish four times less often, but their elapsed
times are essentially unchanged. The write and extraction results do not
establish a substantial speedup.

Source-tree regular extent items fall from 175,626–175,634 to 86,130–86,140.
The first 16 GiB file has 688–689 extents instead of 262,208–262,272. Its
largest extent is 28.75 MiB: reservation-triggered publication still limits
coalescing well before the new 128 MiB cap.

Median tree-deletion device flushes fall from 1,252 to 48, cumulative flush
time from 6.768 to 0.415 seconds, and bytes written from 604.828 to 32.438
MiB. Large-file deletion falls from 1,410 to two flushes and from 568.992 to
1.836 MiB written. The full commit protocol still issues both barriers.

All three final runs verify every byte of all 86,196 archive files after
remount, all ownership/mode changes, and every byte of both 16 GiB files.
The 12 unmounted checkpoints pass independent structural and data-checksum
checks. Grep output hashes match the original comparison.

## Validation and baseline failures

The retained kernel is GENERIC.MP#10, SHA256
`807f5c934d8febab8af901c020edf0f2a0f72e6dd0c122d0b37ef7d32faedb2a`,
with tracing disabled. It passes 75 standard layout/case combinations:
coalescing, reservation drains and their crash rollback, checksums, partial
writes, write-capacity fallback, reflinks, subvolumes/snapshots, NODATASUM,
orphan cleanup, batched truncation, publication-boundary resets, and
unlink/rename/orphan/truncate/ENOSPC capacity tests across the four layouts.
These include independent unmounted checks, metadata mirror checks, and
content/attribute verification after read-only and writable remounts.
Twelve random-reset trials on the bitmap/DUP layout also pass, covering
concurrent append/fsync, namespace operations, and mixed workloads with
host-recorded durability acknowledgements.

The small `16k-bitmap/subvolume` fixture returns ENOSPC when deleting
`container/home` after taking the `latest` snapshot. This occurs with the
experimental kernel and was independently reproduced at exactly the same
operation with the original GENERIC.MP#5 kernel. This fixture uses 256 MiB
with DUP data and metadata; the native subvolume deletion path reserves its
entire deletion before mutation. The experimental failure image passes
`btrfs check --readonly --check-data-csum` after unmount.

The failure image, both command logs, and the independent checker output
are retained under the artifact directory. This is a baseline test failure,
not a passing result for the experimental kernel. Repeating the same complete
snapshot/subvolume recipe on a 1 GiB bitmap/DUP fixture passes with the
experimental kernel, providing a 76th successful validation case.

OpenBSD-written fsync logs for data changes and truncation pass replay and
remount verification with both Linux and OpenBSD at 16 KiB nodes.
The incremental-log test passes in full at 4 KiB. At 16 KiB its aggregate
block-reuse performance assertion fails on both the experimental kernel and
the original GENERIC.MP#5 kernel. The saved experimental 16 KiB incremental
log nevertheless passes independent replay, data checking, and remount
verification with both kernels. The failed assertion is retained in the
logs; the complete 16 KiB incremental test is not counted as passing.
Both kernels produce exactly 186 new blocks over 361 cumulative forest
blocks; the assertion requires fewer than one third to be new.

Crash tests reset the guest while QEMU and the host cache survive. They
exercise publication and recovery boundaries; they do not simulate host
power loss, torn sectors, or loss/reordering of device-cache writes.

## Scope and remaining work

These changes address the persistent extent layout and metadata-only
reservation pressure. They do not implement delayed allocation or coalesce
across already published transactions. During data-heavy writing, retained
metadata reservations still force frequent publication and limit runs well
below 128 MiB. Extraction also retains its namespace scratch-buffer and
per-operation tree-edit costs.

Allowing a data-bearing transaction to drain and reopen needs additional
allocation-accounting work: merged extents must have retirement and
cancellation records matching their new boundaries. Simply lifting the
empty-ordered-queue restriction is incorrect. Further reductions in write
commits should also bound dirty data, dirty metadata, pins, and log-owned
allocations while preserving per-operation ENOSPC guarantees.

Raw benchmark JSONL, tree dumps, checker output, build logs, and validation
artifacts are kept outside the source tree under
`/home/mike/obj/btrfs-improvements/`. `coalesce/` contains the isolated
coalescing run; `combined/` contains the first drain prototype; `refill/`
contains the three final runs. `summary.json`, `layouts.json`, and
`kernel.patch` retain the aggregate measurements, extent distributions, and
tested kernel-source diff. Regression logs are in `regress/`,
`regress-remaining/`, `subvolume-large/`, `random-crash/`, and the log-test
directories. Both baseline controls have separate logs. The experimental
subvolume failure image and both incremental-log crash images are preserved.
