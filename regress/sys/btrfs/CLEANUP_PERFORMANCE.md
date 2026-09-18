# Bounded cleanup without a commit per batch

The September 17, 2026 benchmark rerun stopped after its first Btrfs pass
because deleting the sequential files took minutes. Source-tree deletion
took 103.457 seconds. The FFS2 and FFS2-async comparisons were not run.
The subsequent [full comparison](BENCHMARKS_20260918.md) includes all three
configurations on the patched kernel.

Focused measurements on `5d952a179412` identified excessive intermediate
cleanup commits. The improvements from `41b71f453d2` and `54e27b73dc33` are
still present: cleanup only commits pending data belonging to the removed
inode, small inodes reserve only their remaining work, and completed unlinks
can share a transaction. However, an inode with more than eight extent/xattr
items still forced a full transaction commit after every eight-item batch.
Each commit issues two device-cache flushes.

This investigation did not establish a historical regression point.
It measured and improved the current implementation on the same VM.

## Change and measurements

`btrfs_cleanup_inode()` still bounds each reservation and handle to at most
eight items, reducing that bound under space pressure. Intermediate batches
now share transactions. `btrfs_trans_join()` already commits when retained
reservations or pinned allocations prevent the next reservation from fitting.
That mechanism bounds accumulated work and releases space.

Cleanup still commits promptly when using its minimum protected reservation.
A linked truncate still publishes its final batch before returning. Every
published intermediate state contains consistent inode byte accounting,
remaining mappings and the orphan marker; final unlink removes the inode
and marker together. The device-cache barriers are unchanged.

Times below include `rm` and the following unmount. Each row is one focused
before/after measurement on a freshly formatted filesystem. Commits are the
on-disk superblock generation difference, including the final unmount;
flush counts and cumulative flush times come from QEMU's `info blockstats`.

| Workload | Before, seconds | After, seconds | Speedup | Commits before / after | Flushes before / after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Delete 1 GiB sequential file | 24.106 | 0.560 | 43.1x | 2,048 / 37 | 4,096 / 74 |
| Delete source tree | 100.547 | 14.301 | 7.0x | 7,760 / 625 | 15,520 / 1,250 |

For the 1 GiB file, cumulative device-flush time fell from 21.658 to 0.389
seconds. Metadata writes fell from 729.594 to 14.477 MiB. For the tree,
flush time fell from 82.290 to 6.653 seconds, and metadata writes fell from
3,861.969 to 550.477 MiB. The shorter runtime therefore accompanies a
measured reduction in commits, device flushes and rewritten metadata.

The original 16 GiB file size was also checked with the modified kernel:
deletion including unmount took **9.998 seconds**, with 620 commits and
1,240 flushes. The original runner did not time its sequential-file cleanup,
so there is no precise before/after ratio for that size. `benchmark.py` now
records that cleanup as `delete-large-*` to expose its cost in future runs.

## Setup and reproduction

The guest runs OpenBSD 8.0-beta GENERIC.MP with eight vCPUs, 8 GiB RAM and
`kern.bufcachepercent=20`. Baseline kernel #1 was built from `5d952a179412`;
kernel #2 adds the cleanup commit-condition change. No old kernel was booted.
Builds and other guest tests did not overlap the focused measurements.

Both kernels used `/dev/sd1c`, a 100 GiB virtio-scsi disk backed by
`/home/mike/obj/vm/scratch1.img`. QEMU uses writeback caching. The Linux host
uses Btrfs; the scratch image has the NOCOW attribute. Host caches were not
dropped. These are VM measurements, not physical-device throughput claims.

Formatting was identical for every workload:

```
mkfs.btrfs -f -K -n 16384 -s 4096 -d single -m dup \
    -O free-space-tree,^block-group-tree,no-holes \
    --checksum crc32c /dev/sd1c
mount -t btrfs -o rw /dev/sd1c /mnt/bench
```

The file workload writes zeroes with `dd bs=1m count=1024`, or `count=16384`
for the larger follow-up. Compression is disabled. The source workload
extracts a local, uncompressed `git archive` of `5d952a179412` with `tar -xpf`:
86,196 files, 7,254 directories, no symlinks, and 1,544,684,493 file bytes.
Archive SHA256:
`01421823fdbd42e7268a711e980563782a87f0e08b53712b7533d1a35ab070fd`.

Each workload is unmounted and checked with
`btrfs check --readonly --check-data-csum` before timing. The runner records
the superblock generation and QEMU counters, mounts the filesystem, times
`rm -rf` followed by unmount, and records both counters again. It checks
the resulting filesystem independently and verifies the deleted path is
absent after read-only remount. All five measured workloads passed the
filesystem checks before and after deletion.

The focused source-tree measurements delete immediately after extraction
and checking. The original 103.457-second run also performed grep,
ownership/mode changes and sequential I/O before deleting the tree, so it is
reported separately from the controlled 100.547-second baseline.

Raw JSON, command/check logs and the focused runner are in
`/home/mike/obj/btrfs-rm-investigation/`, using `before`, `after` and
`after-16g` prefixes. Run `cleanup_bench.py TAG` there for the 1 GiB and tree
workloads, or `cleanup_bench.py TAG --workloads large --mib 16384`.
The script reformats the disposable `sd1c` and records the running kernel.
The aborted benchmark's archive and logs remain in
`/home/mike/obj/btrfs-bench-20260917/`.

## Correctness validation

Validation completed on September 18 with the same patched kernel #2.
All 32 selected regression cases passed: `orphan_cleanup`, `truncate_batches`,
`unlink`, `rename`, `recover-orphan-mount`, `recover-truncate-batches`,
`orphan-cleanup-capacity`, and `truncate-batches-capacity`, each on the
`4k`, `16k`, `4k-dup`, and `16k-bitmap` layouts.

These cover 4 KiB and 16 KiB metadata, SINGLE and DUP data, absent/extent/bitmap
free-space trees, a separate block-group tree, explicit holes, open unlinked
files, hard links, xattrs, mapped reads, metadata exhaustion and protected
cleanup reservations. The recovery cases reset the VM during cleanup
publication, including another reset during mount-time orphan recovery.
The runner independently checks filesystem structure, data checksums and
metadata mirrors, and verifies surviving data after remount.

The existing `orphan_ref_stress.py` also passed with eight workers, four rounds
and four open orphans per worker per round: **128 concurrent shared-extent
cleanups** while other files remained live and were modified. All anchor and
survivor byte models passed immediately and after read-only and writable
remounts. Independent filesystem/checksum and metadata-mirror checks passed.

Logs are in `regress/` and `regress-resumed/` beneath the investigation
directory; `regression-summary.json` combines the 32 passing results.
The split records a user-requested VM shutdown after 14 completed cases.
Only the remaining 18 cases were rerun after restart. The regression sources
and kernel were unchanged; the benchmark runner's added large-file cleanup
timing required a new suite-results directory. Concurrent-test logs and its
driver are `shared-cleanup-commands.log` and `shared_cleanup.py`.
