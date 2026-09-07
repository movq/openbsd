# Performance architecture investigation

Investigated at `a5654709607`, using the existing OpenBSD guest: GENERIC.MP
#129, eight vCPUs, 1 GiB RAM, `kern.bufcachepercent=20`, virtio-scsi, and
host-cached sparse scratch images. Linux source at `09f35c36a4ad0` was read
for architectural comparison; no Linux code was imported.

The present gap does not establish an inherent btrfs performance limit.
Both sector-level metadata work and the interaction with OpenBSD's buffer
mapping machinery remain substantial costs. Half of FFS2 throughput is a
useful goal for these particular workloads, not a universal filesystem
guarantee. Evaluate each workload independently with the same durability,
cache, checksum, compression, sharing, and fragmentation conditions.

## Fresh sequential comparison

Each pass reformatted an unmounted 100 GiB scratch filesystem. Btrfs used
16 KiB nodes, 4 KiB sectors, SINGLE data, DUP metadata, extent-format
free-space records, NO_HOLES, and no separate block-group tree. FFS2 used
16 KiB blocks and 2 KiB fragments. Mounts used `noatime`; writes include
unmount. Each command transferred 2 GiB with `dd bs=1m`. The first read
followed remount; the repeat immediately followed it.

| Workload | FFS2 before / after, seconds | Btrfs, seconds | Btrfs / FFS2 throughput |
| --- | ---: | ---: | ---: |
| Write, including unmount | 3.057 / 3.176 | 17.337 | 0.18x |
| Read after remount | 1.815 / 1.925 | 6.423 | 0.28–0.30x |
| Immediate read repeat | 1.902 / 2.047 | 6.781 | 0.28–0.30x |

These are uninstrumented passes, with one btrfs sample bracketed by two
FFS2 controls. The btrfs write consumed 13.90 seconds of child system CPU,
including unmount, and advanced 1,160 generations. The remounted read
consumed 5.55 seconds of system CPU. Thus device wait alone cannot explain
the gap. A 0.5x target here means roughly 6.1–6.4 seconds for the write and
3.6–3.8 seconds for the remounted read.

The 2 GiB file exceeds guest RAM and buffer-cache capacity. Neither the
immediate repeat nor the remounted pass is a fully memory-resident test;
remount does not clear host caches. `ru_inblock` also counts logical
file-buffer misses, so it must not be interpreted as physical disk IOPS.

## CPU sampling

Separate passes sampled all CPUs at 199 Hz. The table conditions on stacks
containing a btrfs function and reports inclusive counts. Nested rows overlap
and must not be added. Samples locate CPU work, not time asleep waiting for
I/O. Stack truncation, interrupt frames, and short workloads limit precision.

| Workload | Btrfs samples | Selected inclusive paths |
| --- | ---: | --- |
| Write 2 GiB | 2,727 | `pmap_kremove` 35.5%; transaction commit 34.5%; sector mutation 29.1%; `buf_unmap` 24.2% |
| Read after remount | 1,193 | `buf_unmap` 54.4%; tree search 9.7% |
| Read repeat | 1,237 | `buf_unmap` 49.7%; tree search 8.7% |
| Extract source tree | 3,178 | write path 68.9%; `pmap_kremove` 37.9%; transaction commit 32.8% |
| Grep after remount | 1,425 | VM pager `uvn_io` 87.7%; `buf_unmap` 50.5% |
| Grep repeat | 1,309 | VM pager `uvn_io` 84.4%; `buf_unmap` 47.1% |
| Chown / chmod | 306 / 325 | transaction commit about 40%; `pmap_kremove` about 30% |
| Delete source tree | 1,904 | transaction commit 44.3%; `pmap_kremove` 42.0%; `km_free` 36.3% |

Sampled sequential writes took 17.474 seconds, versus 17.337 without tracing;
sampled reads took 6.865 / 7.083 seconds. These separate passes measure
tracing plus run variation, not a calibrated instrumentation overhead.

The source corpus is the full archive described in [BENCHMARKS.md](BENCHMARKS.md).
Sampled extraction took 21.889 seconds, grep 14.609 / 15.447, chown 2.465,
chmod 2.396, and deletion 12.317. Deletion advanced 1,464 generations.
There are no fresh FFS2 tree controls here; these timings are profiling
context, not a replacement comparison. In particular, the original
19-minute deletion result no longer describes HEAD.

The striking common cost is virtual-memory mapping turnover. In
`sys/kern/vfs_biomem.c`, each mapped buffer occupies a `MAXPHYS` virtual
address slot regardless of its actual size. Reusing a slot unmaps its old
buffer. On amd64, removing kernel mappings sends TLB invalidation IPIs to
other running CPUs. In this eight-vCPU guest, `x2apic_ipi` alone accounts
for 38% of the remounted btrfs read samples and 25% of write samples.
Large temporary allocations also reach `pmap_kremove` through `km_free`.

FFS2 pays the same underlying cost: a sampled remounted read had 158 of
270 FFS stacks under `buf_unmap`, versus 649 of 1,193 btrfs stacks.
At 199 Hz those counts correspond to approximately 0.79 and 3.26 CPU
seconds. The similar percentages conceal much more absolute work in btrfs.
The VM may amplify IPI costs; a one-vCPU sensitivity experiment would help
separate that effect, but was not performed.

## Architectural comparison and priorities

| Area | OpenBSD btrfs at the investigated HEAD | Linux btrfs |
| --- | --- | --- |
| Buffered writes | One handle, reservation, allocation, checksum edit, file mapping and inode update per sector; clean vnode buffers plus pending payload copies | Page-cache writes reserve space and mark delayed-allocation ranges; writeback allocates extents and ordered completion installs metadata |
| Reads | Sector vnode buffers; mapping and checksum searches on misses; separate physical windows up to 64 KiB | Page cache, extent-map cache, readahead and batched bio submission |
| Data extents | Sector mappings merged at commit into at most 64 KiB extents | Allocation and ordered work operate on larger ranges, independently of individual bio limits |
| Commit and I/O | New joins wait through publication; data and metadata submissions synchronously wait, including each required mirror | Asynchronous I/O; new transactions can start after root switching while the old transaction writes metadata and superblocks |
| Inode metadata / fsync | Immediate inode-item edits; fsync can commit the shared transaction | Delayed inode items; log tree can satisfy fsync without a full commit, with fallback |
| Concurrency | Vnode operations enter under the kernel lock; additional root and namespace serialization | Finer locking and parallel writeback workers |

The relevant Linux paths are `file.c:btrfs_buffered_write`,
`inode.c:run_delalloc_cow` and `btrfs_finish_one_ordered`,
`extent_io.c:btrfs_writepages` and `btrfs_readahead`, and the
`TRANS_STATE_UNBLOCKED` transition in `transaction.c`.
OpenBSD FFS already uses `bread_cluster`, `bawrite`, and `bdwrite`;
the kernel supports batching and asynchronous I/O that this driver does
not yet exploit fully.

Recommended order:

1. **Reduce mapping and allocation churn.** Design larger logical read
   windows or a range read path that avoids constructing a second cache of
   4 KiB buffers. Include VM-pager reads, pending writes, EOF, compressed
   extents, and sector-granular mirror recovery. Reuse bounded staging and
   metadata storage where profiling shows repeated allocation/free costs.
   Merely skipping `bread` for full-sector appends still leaves buffer
   allocation and mapping; the previous unsuccessful experiment does not
   rule out this redesign.
2. **Make pending writes and reservations operate on bounded ranges.**
   Create larger mappings directly, batch checksum edits, and update the
   inode once per range. Reservation bounds should reflect affected items
   and shared tree paths rather than multiply a worst-case sector budget.
   Preserve partial-copy failure, pending overwrite/cancellation, reflink
   ownership, protected cleanup reserves, and abort semantics. This also
   removes work currently spent constructing and then coalescing sector
   items. Simply reducing reserve constants is not an adequate design.
3. **Queue bounded data and metadata I/O with explicit completion.**
   Retain immutable payload ownership until completion and preserve all
   mirror and barrier requirements. Profile wait time and queue depth to
   decide how much concurrency helps after reducing CPU work.
4. **Reassess delayed allocation, delayed inode items and transaction
   overlap.** Delayed allocation can combine separate small writes; overlap
   requires per-generation roots, pins and buffer ownership. These are
   larger changes. The current single-process workloads do not establish
   kernel-lock contention as their primary bottleneck. A log tree matters
   for frequent fsync, but cannot explain these bulk-unmount measurements.

The experiments below test the first and third recommendations. Range-based
pending ownership and reservations remain the next architectural step. Record
elapsed/system time, generations, sampled stacks and actual device I/O separately;
do not infer throughput from an inclusive CPU percentage.

## Implementation experiments

Cumulative prototypes used the same guest and sequential workload, with tracing
disabled. Writes include unmount; reads follow remount. Each row below is the
first pass at that stage, so small differences are not independently established
effects.

| Prototype | Kernel | Write seconds | Write system seconds | Read seconds |
| --- | --- | ---: | ---: | ---: |
| HEAD control | #129 | 17.209 | 13.99 | 6.312 |
| Direct bounded range reads | #130 | 17.656 | 13.96 | 1.960 |
| Also remove vnode buffers from writes/truncate/inline clone | #131 | 12.066 | 8.34 | 1.785 |
| Also reuse scratch and metadata storage in pools | #132 | 12.242 | 8.39 | 1.738 |
| Also queue bounded asynchronous physical writes | #133 | 10.643 | 8.27 | 1.819 |

Range reads stage up to `MAXBSIZE`, consult pending data first, and retain
sector checksums and mirror recovery. Mutation paths use temporary sector
copies directly; a full-sector overwrite still preserves the old contents
before `uiomove`, since a partial copy can fail. The strategy compatibility
path no longer retains a second cache of file data.

The asynchronous queue permits at most 16 outstanding physical writes per
commit phase, including DUP mirrors. Each submitted buffer owns its bytes.
Both successful and failed phase exits drain all completions. Data completes
before coalescing and metadata work; metadata completes before the first
barrier. Superblock publication and both cache barriers retain their ordering.

All stages advanced 1,160 generations for the 2 GiB write. These changes reduce
buffer/allocation cost and I/O wait; they do not reduce sector mutation work or
reservation-driven commits. Range ownership requires coordinated changes to
pending lookup, checksum insertion, truncation/splitting, cancellation,
reflink references, and reservation bounds. No reservation constants were
reduced in this experiment.

Pool reuse alone did not improve the initial sequential write. A later removal
comparison kept range reads and asynchronous writes and changed only the pools:

| Workload | With pools, #133 | Without pools, #134 |
| --- | ---: | ---: |
| Extract source tree | 15.206 | 16.470 |
| Chown | 1.642 | 2.006 |
| Chmod | 1.669 | 2.235 |
| Delete source tree | 10.608 | 12.239 |
| Write 2 GiB | 11.404 | 12.600 |
| Write system CPU | 8.71 | 10.13 |
| Delete system CPU | 8.35 | 10.38 |

Each column used a clean tree pass followed by deletion and a sequential pass,
with no concurrent kernel build. The earlier `experiment-pools-tree` pass
overlapped a build and is excluded from performance conclusions. The CPU
reduction in this comparison supports retaining the pools along with the
buffer and asynchronous-I/O changes.

### Retained build and fresh controls

After restoring the pools, #135 passed two fresh sequential iterations and a
full source-tree pass, bracketed by FFS2 controls. A fresh uninstrumented #129
pass supplies the HEAD column. Times are seconds; mutations include unmount.
The two sequential values are separate fresh filesystems, while each btrfs tree
column has one pass. No performance pass below overlapped a kernel build.

| Workload | HEAD #129 | Retained #135 | FFS2 before / after |
| --- | ---: | ---: | ---: |
| Write 2 GiB | 17.513 | 10.509 / 10.820 | 3.593 / 3.552 |
| Read after remount | 6.887 | 1.799 / 1.853 | 2.385 / 2.364 |
| Immediate read repeat | 6.976 | 1.769 / 1.899 | 2.593 / 2.432 |
| Extract source tree | 23.493 | 16.048 | 13.632 / 11.214 |
| Grep after remount | 15.390 | 10.889 | 11.789 / 11.447 |
| Immediate grep repeat | 15.953 | 11.394 | 11.909 / 11.809 |
| Chown | 2.433 | 1.748 | 0.948 / 0.904 |
| Chmod | 2.352 | 1.980 | 0.901 / 0.897 |
| Delete source tree | 14.164 | 10.939 | 4.039 / 3.981 |

The retained build reaches 0.33–0.34x FFS2 write throughput and 1.28–1.33x
remounted-read throughput here. Extraction reaches 0.70–0.85x, remounted grep
1.05–1.08x, chown 0.52–0.54x, chmod about 0.45x, and deletion about 0.36–0.37x.
Thus the 0.5x target is met for reads, extraction, grep and chown; chmod is
borderline, while bulk writes and deletion remain below it.

Compared with the fresh HEAD pass, sequential writes are 1.62–1.67x faster
and remounted reads 3.72–3.83x faster. Write system CPU falls from 13.99 to
8.25–8.53 seconds; read system CPU falls from 5.85 to 1.18–1.22 seconds.
Tree deletion still advances 1,464 generations, and the populated-tree phases
still advance 1,060. The remaining write/deletion gaps support investigating
reservation and mutation batching next. Host caches remain warm, and the FFS2
extraction variation illustrates why these VM results are workload-specific.

### Correctness and recovery

Correctness checks include unaligned reads across 4 KiB/64 KiB boundaries,
pending overwrite, holes, truncate/regrow, EOF, VM-pager reads, compressed
imports, reflinks, inline conversion, checksums, and space exhaustion. The
new `read-range` case also uses a valid first `writev` iovec followed by an
invalid one to check preservation of the untouched part of a sector after
`EFAULT`. The damaged-DUP test includes an unaligned read whose sectors need
different healthy mirrors.

The retained build (#135) passed 21 targeted cases: checksums, coalescing,
reflinks and `read-range` on 4 KiB, 16 KiB and 16 KiB bitmap/DUP layouts;
namespace, imported reads, compressed reads, damaged mirrors, inline files,
growth, shrinking and both publication recovery cases on 16 KiB. Earlier
stages also passed their affected operation classes, including ENOSPC on
the asynchronous build. The full suite was not run.

`publish-before` and `publish-after` passed with asynchronous I/O, recovering
the old and new committed contents respectively. Four additional DDB
experiments on #134 changed completion status at `btrfs_write_done`: `B_ERROR`
with `EIO`, or a nonzero residual without `B_ERROR`, in each of the ordered-data
and metadata phases. All four returned `EIO` from `fsync`, rejected further
writes with `EROFS`, and preserved the previous committed contents through
reset, independent checks, and read-only/read-write remount. These inject
completion failures; they do not emulate lost device-cache contents.

Experiment logs use `experiment-*` prefixes in the results directory below.
Targeted test logs are in
`/home/mike/obj/btrfs-{range-read,bufferless,pools,async,retained,retained-formats}-regress`.
The completion-fault runner is `/home/mike/obj/btrfs-write-fault.py`, with
`btrfs-fault-{data,metadata}-{short,error}` result directories. Its amd64
`struct buf` offsets were checked using `btrfs-buf-offsets.c` against the guest
headers before injection; recheck offsets before using it on another build.

## Reproduction and checks

Enable `kern.allowdt=1` at boot for profiling. The working sampling action is:

```text
profile:hz:199 {
        @stacks[kstack] = count();
        @commands[comm] = count();
}
```

Start btrace before the command, stop it with SIGINT after the timed
unmount, and retain its output. Use a host `timeout` around guest commands.
The guest's btrace produced empty stacks for `@[comm,kstack]`; separate
maps above produced valid stacks. The earlier `btrfs-sampled` artifacts
are therefore excluded from the CPU analysis. GENERIC.MP also exposed no
kprobes; do not assume function entry/return probes are available.

Raw timing, stack, formatting, and check logs are in
`/home/mike/obj/btrfs-architecture-profile`; the host runner is
`/home/mike/obj/btrfs-architecture-profile.py`, with `summarize.py` in the
results directory. All btrfs passes passed unmounted
`btrfs check --readonly --check-data-csum` and metadata-mirror checks.
Sequential files were also compared completely with zeroes after read-only
remount. The populated tree was independently checked before deletion;
deletion was verified by remount. FFS2 passes passed `fsck_ffs -fn`.
The initial profiling investigation changed no driver code. Implementation
experiments and their targeted correctness checks are described above.
