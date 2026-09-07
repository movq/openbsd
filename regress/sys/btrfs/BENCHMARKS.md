# Btrfs / FFS2 performance baseline

Measured at `4fa9086644e` using `benchmark.py`. Times are elapsed seconds;
smaller is better. FFS2 values show control passes before and after btrfs.
Btrfs has one sample per tree workload. Sequential I/O values average two
iterations per pass. The second FFS2 pass was substantially slower, so ratios
span both controls. These are exploratory measurements.

| Workload | FFS2 seconds, before / after | Btrfs seconds | Btrfs / FFS2 time |
| --- | ---: | ---: | ---: |
| Extract source tree | 9.730 / 19.175 | 63.973 | 3.3–6.6x |
| Recursive grep after remount | 9.768 / 16.731 | 20.729 | 1.2–2.1x |
| Immediate grep repeat | 10.533 / 17.302 | 19.936 | 1.2–1.9x |
| Recursive chown | 0.831 / 1.351 | 3.740 | 2.8–4.5x |
| Recursive chmod | 0.795 / 1.234 | 3.736 | 3.0–4.7x |
| Delete source tree | 3.731 / 5.264 | 1171.600 | 222.6–314.0x |
| Write 2 GiB | 3.452 / 4.480 | 63.877 | 14.3–18.5x |
| Read 2 GiB after remount | 2.266 / 3.564 | 12.801 | 3.6–5.6x |

Deletion is the largest gap: 19 minutes 32 seconds for btrfs versus 4–5 seconds
for FFS2. Sequential writes are also particularly slow, at 32 MiB/s for btrfs.

The workload corpus is an uncompressed source archive containing 86,160 files
and 7,261 directories, totaling 1.438 GiB of file data. Each pass starts on a
fresh filesystem mounted with `noatime`. The runner extracts with `tar -xpf`,
scans all file contents with `grep -r -a -F -c Copyright`, changes ownership
with `chown -R 12345:12345` and modes with `chmod -R u=rwX,go=rX`, writes and
reads 2 GiB files with `dd bs=1m`, then deletes the tree with `rm -rf`.
Btrfs uses uncompressed, checksummed data and duplicated metadata.

Mutation times include the following unmount to account for writeback.
Mounting, formatting, and verification are excluded. Each workload starts
after remount except the immediate grep repeat; remounting clears filesystem
caches, but does not clear host caches. Deletion was measured separately on
a recreated tree with the same ownership and modes.

All six grep outputs agreed after sorting. Remount checks verified corpus
counts, sizes, ownership, and modes. Btrfs checksums and metadata mirrors
passed independent checks on the recreated tree and after deletion; both
FFS2 passes passed `fsck_ffs -fn`.

Raw timings, output comparisons, and independent filesystem checks are in
`/home/mike/obj/btrfs-bench-metadata-baseline`.

## Deletion transaction batching

A smaller `bin` + `sbin` archive was measured before and after removing
unconditional commits from last-close cleanup, starting at `93bca8578a2`.
It contains 1,003 files and 221 directories, with 14,187,356 bytes of file data.
Each of three passes per kernel reformatted the 100 GiB scratch disk with
16 KiB nodes, SINGLE data, DUP metadata, an extent-format free-space tree,
NO_HOLES, and no separate block-group tree. Extraction and deletion used
separate `noatime` mounts. Times include deletion's unmount.

| Kernel | Delete seconds, three passes | Median seconds | Generation advances per pass |
| --- | ---: | ---: | ---: |
| Before | 10.063 / 10.748 / 10.960 | 10.748 | 2,451 |
| Batched cleanup | 0.411 / 0.464 / 0.436 | 0.436 | 21 |

The median improves **24.7x**. Previously, every final unlink/rmdir committed
before cleanup and again after each cleanup batch. Cleanup now commits first
only when the inode has pending ordered data, and a completed deletion can
remain in the shared transaction. Intermediate batches and minimum-reserve
cleanup still commit. Reservation pressure also limits transaction size.
This result applies to the small corpus; the full source tree has not been
remeasured.

Every pass passed unmounted `btrfs check --readonly --check-data-csum`,
metadata-mirror checks, and a read-only remount confirming deletion.
The archive, raw timings, superblock generations, and check logs
are in `/home/mike/obj/btrfs-delete-performance`; the host runner is
`/home/mike/obj/btrfs-delete-bench.py`.

For a short repeat using `benchmark.py`, create an archive with
`tar -cf small.tar bin sbin`, copy it to the guest, and supply `--delete-only`
alongside the usual device, mountpoint, `--type`, `--archive`, and `--out`
arguments. This measures extraction and deletion and skips the other workloads.
Format the unmounted scratch filesystem before each pass and run independent
checks afterward, as for the baseline.

A follow-up sizes each cleanup reservation from the validated number of
remaining mappings and xattr items, retaining the existing minimum budget.
A lookahead also finishes exact multiples of the batch size without an extra
inode/marker-only transaction. A fresh comparison against `30ddf4f0b45`, using
the same archive and procedure, gives:

| Kernel | Delete seconds, three passes | Median seconds | Generation advances per pass |
| --- | ---: | ---: | ---: |
| Batched cleanup, repeat | 0.338 / 0.337 / 0.348 | 0.338 | 21 |
| Counted cleanup | 0.257 / 0.268 / 0.267 | 0.267 | 6 |

This reduces elapsed time by **21%** in the fresh comparison. A small inode's
cleanup now reserves 4 MiB instead of 18 MiB on this layout. The allowance per
actual item remains conservative, so reservation pressure still causes commits.
All six passes passed the same independent and remount checks; their logs use
the `reservations-before` and `reservations-after` prefixes in the results
directory above.

## Sequential-write improvements

Starting at `bf686c4b29e`, a targeted `dd bs=1m count=2048` comparison used a
fresh 100 GiB scratch filesystem per pass: 16 KiB nodes, 4 KiB sectors, SINGLE
data, DUP metadata, extent-format free-space records, NO_HOLES, and no separate
block-group tree. Each write used a `noatime` mount; elapsed time includes
unmount. These runs omit the source-tree workload and do not constitute a new
FFS2 comparison.

| Kernel | Write 2 GiB seconds | Median seconds | Median MiB/s | Generation advances |
| --- | ---: | ---: | ---: | ---: |
| HEAD before changes | 48.322 / 49.098 | 48.710 | 42.0 | 1,160 |
| Packed leaf edits, reservation reuse, accelerated CRC32C | 17.569 / 18.569 / 18.931 | 18.569 | 110.3 | 1,160 |

The median improves **2.6x**. Neither row contains timing instrumentation.
The two baseline logs are `baseline` and `baseline-profile`; the attempted
tracer in the latter failed before the workload started. Final samples use
the `final` prefix.

Temporary elapsed-time counters exposed the main costs. In a 51.978-second
instrumented baseline, transaction joins took 22.8 seconds, including commits
triggered by reservation pressure. Sector mutation took 24.4 seconds, including
8.2 seconds editing checksum items. Commit-time extent coalescing took another
8.3 seconds within the join time. These nested measurements must not be added
together.

Three cumulative experiments reduced that work:

| Instrumented configuration | Seconds | Finding |
| --- | ---: | --- |
| Baseline | 51.978 | Variable-size item edits rebuild whole leaves |
| Edit packed COW leaves in place | 27.060 | Checksum edits fall to 0.8 s; extent coalescing to 1.2 s |
| Also reuse reservation objects while scanning full groups | 23.761 | Avoid allocating/freeing an object for every exhausted group |
| Also accelerate data and metadata CRC32C on amd64 | 17.539 | Data checksum computation falls from 4.3 s to 0.24 s |

The leaf change retains validation and ancestor-key updates, and falls back to
scratch rebuilding for gaps or aliased inputs. The CRC32 instruction uses
general-purpose registers; CPUs without SSE4.2 and other architectures retain
the portable implementation. Reservation sizes and durability ordering are
unchanged. A further experiment skipping buffer reads for full-sector appends
took 17.802 seconds and was dropped because it showed no throughput gain.

Every performance pass passed unmounted `btrfs check --readonly
--check-data-csum`, metadata-mirror checks, and a read-only remount comparing
the complete file with zeroes. The final kernel also passed namespace, rename,
checksums, coalesce, reflink, and xattrs on the `4k`, `16k`, and `16k-bitmap`
layouts: 18 targeted cases with independent checks and remount verification.
A host check of the actual CRC function compared both dispatch paths with an
independent bitwise oracle, including unaligned inputs, short tails, and
inaccessible pages immediately after the input.

Reservations still retain a conservative metadata allowance per sector, and
the filesystem still advances 1,160 generations for this write, including
chunk growth. Per-sector handles and tree operations, plus synchronous 64 KiB
data submissions, remain targets. Bounded range writes could amortize metadata
reservations and create larger mappings directly; that requires preserving
pending replacement, cancellation, and shared-extent ownership rules.

Raw timings, check logs, and the CRC check are in
`/home/mike/obj/btrfs-write-performance`; the host runner is
`/home/mike/obj/btrfs-write-bench.py`. To run only sequential I/O with the
in-tree runner on a freshly formatted, unmounted scratch filesystem:

```sh
timeout 300 ssh root@10.77.0.2 \
    'python3 /mnt/src/regress/sys/btrfs/benchmark.py sequential \
    /dev/sd1c /mnt/bench --type btrfs --io-only --out /root/sequential-results'
```

This performs two write/read/delete iterations. `--io-mib` and
`--io-iterations` allow shorter experiments; `--archive` is unnecessary with
`--io-only`. Formatting and independent checks remain external. The new mode
was exercised with a 16 MiB file and checked independently afterward.
