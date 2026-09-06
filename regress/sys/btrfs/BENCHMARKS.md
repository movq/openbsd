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
