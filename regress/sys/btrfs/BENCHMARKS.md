# Basic performance comparison

This is a small benchmark of the OpenBSD btrfs driver against FFS2, not a
performance regression suite. The workload runner is `benchmark.py`; filesystem
formatting and independent checks are external to it.

## Results

The run was stopped at the user's request during the untimed btrfs tree
verification, after chmod. No btrfs sequential-I/O or deletion measurement
completed. There was one completed FFS2 pass and one partial btrfs pass;
the planned FFS2 control repeat was canceled. These are exploratory samples,
not statistical estimates.

| Workload | FFS2 seconds | Btrfs seconds | Btrfs / FFS2 time |
| --- | ---: | ---: | ---: |
| Extract source tree | 9.697 | 158.432 | 16.3x |
| Recursive grep after remount | 9.996 | 283.922 | 28.4x |
| Immediate grep repeat | 10.560 | 452.939 | 42.9x |
| Recursive chown | 0.785 | 265.785 | 338.6x |
| Recursive chmod | 0.758 | 287.528 | 379.3x |
| Delete source tree | 3.961 | not run | — |
| Write 2 GiB, first / second | 3.472 / 3.418 | not run | — |
| Read 2 GiB, first / second | 2.396 / 2.291 | not run | — |

The FFS2 large-file results correspond to 594 MiB/s writes and 874 MiB/s
reads, using total bytes divided by total time across the two iterations.
All mutation times include unmount.

The completed grep outputs agreed on all 86,160 per-file counts after sorting;
the sorted output SHA256 was
`e3ed025658171b304584effb2e2feb002650b60f3476523446dcdc83ad8da3a0`.
After interruption, the btrfs filesystem was cleanly unmounted and
`btrfs check --readonly --check-data-csum` passed on the populated image.
The full ownership/mode verification was interrupted, so it is not claimed
as completed. No panic occurred. Raw logs, the archive and machine settings
are retained in `/home/mike/obj/btrfs-bench-20260906`; the populated btrfs
tree remains on `scratch1.img`, unmounted.

## Setup

Measured on 2026-09-06 at commit
`0c7eaeb694816fee3147766a933dbeb03ff9d64e`, with the matching OpenBSD
8.0-beta GENERIC.MP #116 kernel. The guest had eight virtual CPUs, **1 GiB RAM**,
and `kern.bufcachepercent=20`. `kern.maxvnodes` was 37,507; btrfs metadata
traversals reached that limit. The RAM allocation was intentionally retained
for this comparison after noticing that it was smaller than intended.

Both filesystems used separate 100 GiB sparse raw disks on the same host NVMe
storage (ext4 over dm-crypt), passed through QEMU virtio-scsi.
The host had about 27 GiB RAM.
QEMU 10.2.2 used writeback disk caching; host caches were not flushed.
Consequently, these are results for this virtualized setup, not physical
drive throughput measurements.

* FFS2: `newfs -O 2 -b 16384 -f 2048 /dev/rsd2c`.
* Btrfs: Alpine Linux btrfs-progs 6.17.1,
  `mkfs.btrfs -f -s 4096 -n 16384 -d single -m dup
  -O free-space-tree,no-holes,^block-group-tree /dev/sdb`.
  CRC32C, uncompressed data, duplicated metadata, default initial chunk sizes.
* Both mounted read/write with `noatime`, without `async` or `sync`.
  OpenBSD's `softdep` mount option is a compatibility no-op.
* Linux did not mount the scratch filesystem during the OpenBSD measurements.

## Workloads and timing

The corpus was an uncompressed `git archive --format=tar` of the measured
commit, staged on the guest root FFS filesystem before timing. This excludes
`.git`, untracked files and build products, and avoids NFS in the data path.
It contains 86,160 regular files, 7,261 directories and no symlinks, totaling
1,544,220,378 file bytes (1.438 GiB). The archive's SHA256 is
`b6a0fddc18ec00f95e8da23f0bc5461fa6161d18efe6a7d3beb6ceb7d4445eb1`.

The runner performs these operations, serially:

1. Extract the entire archive with `tar -xpf` into an empty directory.
2. After unmount/remount, run
   `LC_ALL=C grep -r -a -F -c Copyright tree`, then repeat immediately.
   This scans all file contents, including binary files, and writes per-file
   counts to a file on the guest root disk. The repeat is not an assertion
   that the corpus fits in cache.
3. After separate remounts, run `chown -R 12345:12345 tree` and
   `chmod -R u=rwX,go=rX tree`. The archive contains modes 0664/0775, so chmod
   actually changes the modes to 0644/0755. Verify counts, sizes, ownership
   and modes after another remount, outside the timed interval.
4. Twice, create a new 2 GiB file using
   `dd if=/dev/zero of=large bs=1m count=2048`; unmount/remount and read it with
   `dd if=large of=/dev/null bs=1m`. Remove it outside the timed interval.
   Btrfs compression is disabled, so the zero writes allocate real extents.
5. After another remount, delete the tree with `rm -rf`.

Mutation times include the command **and the following unmount**, to account
for pending writeback. Formatting, mounting and verification are excluded.
The JSONL logs separately record command time, unmount time and child-process
CPU time. The CPU counters do not include the subsequent unmount or work
charged to background kernel threads.

Unmount/remount removes filesystem vnode and driver caches; it does not clear
the host cache, and mounting itself reads metadata. Both the tree and large
file exceed guest RAM. These are single-process workloads, not concurrency
or scaling tests, and the two filesystems provide different features and
durability machinery.

## Baseline interpretation

The large-tree results point to substantial kernel CPU overhead. Extraction
used 146.7 seconds of child system CPU; the first and repeated greps used
275.6 and 442.2 seconds; chown and chmod used 258.2 and 280.1 seconds.
Mutation drain times were small: 0.22 seconds for extraction, 0.11 for chown,
and 0.19 for chmod. FFS2 completed the metadata changes in under a second.

The immediate grep repeat was slower than the run after remount, with
identical results. `vmstat` samples showed no swap-out activity, and the
metadata-only traversals had about 825–830 MiB free while reaching the vnode
limit. More RAM alone is unlikely to resolve the metadata gap.

Two concrete candidates for subsequent profiling are the linear searches in
`btrfs_node_lookup()`/`btrfs_node_insert()`, and the repeated extent/checksum
lookups and metadata validation in the sector-sized read path.
`btrfs_extent_buffer_put()` frees the logical metadata object on its last
reference; a later lookup can repeat validation of physically cached bytes.
These observations identify places to investigate, not measured attribution
of the total runtime. No driver code was changed for the baseline.

## Metadata follow-up

After stopping the original run, three changes were measured individually
on the retained corpus, still with 1 GiB RAM and a remount before each test:

| Cumulative changes | Full-tree chown, including unmount |
| --- | ---: |
| Baseline | 265.785 s |
| Index inode identities with an RB tree | 11.06 s |
| Also index extent buffers and retain an 8 MiB LRU of validated metadata | 5.27 s |
| Also replace equal-size item payloads directly in private COW leaves | 3.223 s |

The first short kernel profile showed substantial allocation/free and
cross-CPU TLB shootdown cost from rebuilding a complete 16 KiB leaf for each
inode update. The last change avoids that work when the item size is unchanged,
without changing keys, packed layout, or COW ownership.

Final GENERIC.MP #120 measurements were **3.223 s chown** and **3.348 s chmod**,
about 82x and 86x faster than the baseline. Their command system CPU times
were 2.61 s and 2.66 s; final unmounts took 0.121 s and 0.091 s.
Final chmod changed the modes to 0600/0700; the baseline changed them to
0644/0755. Both traversals changed the modes of all archive entries.
All 86,160 files and 7,261 directories were then checked after remount for
ownership, modes, sizes and executable-file count, outside the timed interval.

These remain approximately 4.1x and 4.4x the baseline FFS2 elapsed times.
The follow-up profile had 672 of 1,287 chown samples inside transaction commit,
primarily reached through reservation retry. Further reservation/writeback
changes were deferred at the user's request to keep this optimization pass
bounded. Extraction, grep, deletion and sequential I/O were not rebenchmarked.

Validation passed `4k/namespace`, `4k/subvolume`, `16k/subvolume`, and
`16k/reclaim`, including the runner's independent checks and DUP mirror
verification. The populated corpus also passed a final
`btrfs check --readonly --check-data-csum` and verification of all 8,383
metadata blocks' copies. Temporary dynamic-tracing configuration was removed.
Profiles, intermediate timings, final results and targeted test logs are
alongside the baseline artifacts in `/home/mike/obj/btrfs-bench-20260906`.

## Reproduction

Use disposable, freshly formatted, unmounted scratch filesystems. Stage the
same archive on the guest's root disk and run from the host, for example:

```sh
timeout 3600 ssh root@10.77.0.2 \
  'python3 /mnt/src/regress/sys/btrfs/benchmark.py btrfs-r1 \
  /dev/sd1c /mnt/bench --type btrfs \
  --archive /root/btrfs-bench/source.tar --out /root/btrfs-bench'
```

For FFS2, change the configuration name, device and `--type ffs`.
Each invocation creates and removes `tree` and `large` at the mountpoint.
Use unique configuration names to keep the logs separate. Reformat before
another pass. Do not access a scratch filesystem through the other VM while
it is mounted in OpenBSD.
