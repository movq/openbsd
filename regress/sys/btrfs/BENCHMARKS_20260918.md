# Btrfs / FFS2 comparison, September 18, 2026

Measured with the kernel sources in `68f36cd7fe246`, including the
[cleanup batching fix](CLEANUP_PERFORMANCE.md), against FFS2 with normal
mount options and FFS2 with `async`.

FFS2 `async` improves source extraction by **1.43x** and source-tree deletion
by **4.72x** relative to default FFS2, using median elapsed times. Ownership,
mode changes and sequential I/O show little difference between the FFS mounts.

Btrfs matches warm grep performance and has somewhat faster large-file reads
in this VM. Extraction, ownership/mode changes and large-file writes deliver
about **54–57% of default FFS2 throughput**. Deletion remains the largest gap:
source-tree deletion takes **5.2x** as long as default FFS2 and **24.5x** as
long as FFS2 `async`; deleting the 16 GiB file takes about **45x** as long as
default FFS2, despite the substantial improvement from the cleanup fix.

## Elapsed times

Seconds, lower is better. Each cell is **median (minimum–maximum)**.
Tree workloads have three samples per configuration. Sequential file workloads
have six samples per configuration: two passes on each of three fresh
filesystems. These ranges describe the observations, not confidence intervals.

Mutation timings include the command and its following unmount/writeback.
Read and grep timings measure the command only.

| Workload | Btrfs | FFS2 default | FFS2 async |
| --- | ---: | ---: | ---: |
| Extract source tree | 14.892 (14.687–14.905) | 8.214 (7.916–8.887) | 5.725 (5.131–5.785) |
| Grep after remount | 8.401 (8.319–8.435) | 6.583 (6.575–6.809) | 6.641 (6.626–6.668) |
| Immediate grep repeat | 2.401 (2.401–2.407) | 2.416 (2.394–2.498) | 2.413 (2.411–2.423) |
| Recursive chown | 1.170 (1.152–1.208) | 0.650 (0.636–0.709) | 0.671 (0.654–0.675) |
| Recursive chmod | 1.172 (1.161–1.186) | 0.638 (0.633–0.662) | 0.639 (0.628–0.652) |
| Delete source tree | 14.727 (14.660–14.734) | 2.833 (2.776–3.031) | 0.600 (0.591–0.614) |
| Write 16 GiB | 26.124 (25.399–27.457) | 15.007 (14.759–15.519) | 15.024 (14.414–15.458) |
| Read 16 GiB after remount | 12.845 (12.554–13.214) | 13.780 (13.545–14.769) | 13.953 (13.664–14.993) |
| Immediate 16 GiB read repeat | 12.299 (12.096–12.640) | 14.206 (14.095–16.216) | 14.293 (14.048–15.535) |
| Delete 16 GiB file | 11.282 (11.208–11.617) | 0.250 (0.246–0.284) | 0.258 (0.243–0.262) |

Sequential throughput, computed as 16,384 MiB divided by median elapsed time:

| Workload | Btrfs MiB/s | FFS2 default MiB/s | FFS2 async MiB/s |
| --- | ---: | ---: | ---: |
| Write, including unmount | 627.2 | 1,091.7 | 1,090.5 |
| Read after remount | 1,275.5 | 1,189.0 | 1,174.2 |
| Immediate read repeat | 1,332.2 | 1,153.3 | 1,146.3 |

## Environment and workloads

The guest is OpenBSD 8.0-beta GENERIC.MP#2 on eight vCPUs and 8 GiB RAM,
with `kern.bufcachepercent=20` and the `pvclock0` timecounter. It uses
100 GiB virtio-scsi scratch disks backed by raw host files. QEMU uses writeback
caching; all scratch files have the NOCOW attribute on the Linux host's Btrfs
filesystem. No builds or other test workloads overlapped measurements.

The uncompressed archive is a `git archive` of `5d952a179412`, retained from
the initial benchmark attempt so every pass receives identical input. It
contains 86,196 regular files, 7,254 directories, no symlinks, and
1,544,684,493 file bytes (1.439 GiB). Its SHA256 is
`01421823fdbd42e7268a711e980563782a87f0e08b53712b7533d1a35ab070fd`.
The archive resides on the guest's local root filesystem and is read once
before each run, outside the timings. Timed extraction does not read over NFS.

The workloads in `benchmark.py` are:

* `tar -xpf` into a new `tree` directory.
* `grep -r -a -F -c Copyright`, after remount and immediately again.
* `chown -R 12345:12345` and `chmod -R u=rwX,go=rX`.
* Two sequential passes using `dd bs=1m`, each writing a new 16 GiB file from
  `/dev/zero`, reading it after remount, immediately reading it again, and
  deleting it. Compression is disabled.
* `rm -rf tree`, after the metadata and sequential workloads.

Every timed workload starts after remount except the immediate read/grep
repeats. Formatting, mounting, archive warming, correctness verification and
independent filesystem checks are excluded from timings. The runner also
records command time and unmount time separately in the raw results.

## Formats, mount options and rotation

For each fresh Btrfs filesystem:

```
mkfs.btrfs -f -K -n 16384 -s 4096 -d single -m dup \
    -O free-space-tree,^block-group-tree,no-holes \
    --checksum crc32c /dev/sdNc
mount -t btrfs -o rw /dev/sdNc /mnt/bench
```

This uses 16 KiB nodes, 4 KiB sectors, SINGLE data, DUP metadata, CRC32C data
checksums, extent-format free-space records and no separate block-group tree.

For each fresh FFS2 filesystem:

```
newfs -O 2 -b 16384 -f 2048 /dev/rsdNc
mount -t ffs -o rw /dev/sdNc /mnt/bench
```

The async configuration changes only the mount options to `rw,async`.
Neither configuration uses `noatime` or `softdep`. Actual mount flags are
recorded by the runner for every pass.

Each filesystem used every scratch disk once and occupied every test-order
position once. The disks were tested sequentially, with a fresh format for
every cell:

| Round | First: sd1 | Second: sd2 | Third: sd3 |
| --- | --- | --- | --- |
| 1 | Btrfs | FFS2 default | FFS2 async |
| 2 | FFS2 default | FFS2 async | Btrfs |
| 3 | FFS2 async | Btrfs | FFS2 default |

## Validation and interpretation

All **nine complete runs and 126 measurements** passed verification. Extracted
file contents were compared byte-for-byte with the archive, ownership/modes
and tree counts were checked, and per-file grep counts matched the archive
on every pass. All 18 sequential files were completely compared with zeroes
after remount. Tree deletion was verified after remount.

Each run paused unmounted after extraction, each sequential write, and final
tree deletion. Btrfs passed 12 `btrfs check --readonly --check-data-csum`
checks and 12 independent metadata-mirror checks. FFS2 passed 24
`fsck_ffs -fn` checks across the default and async configurations.

Remount removes the guest filesystem's cached state; host caches were not
dropped. The 16 GiB files exceed guest RAM, so immediate sequential repeats
are not fully memory-resident tests. These are measurements of this VM and
its cached backing storage.

A separate probe on the unused fourth disk confirmed an implementation
difference: Btrfs does not update a regular file's atime on read, while both
FFS2 mounts do, with the new atime persisting after remount. Thus using normal
mount options does not make read-atime work identical across filesystems.
Unmount includes deferred writeback in mutation totals, but the mount
configurations still have different behavior during an interrupted workload.

The historical results in `BENCHMARKS.md` used 1 GiB guest RAM, 2 GiB sequential
files and `noatime`. They are not a controlled before/after comparison with
these runs. The separately measured cleanup improvement is documented in
`CLEANUP_PERFORMANCE.md`.

## Saved artifacts

Host results are in `/home/mike/obj/btrfs-bench-20260918/`, including:

* `timings.csv`, `summary.json` and `verification.json`.
* `r*-logs.tar`: guest JSONL timings, command stderr and grep output.
* `r*-check-*.log`: independent filesystem and mirror checks.
* `commands.log`, `matrix.log`, `environment.log`, `qemu-blocks.txt` and
  `host-image-attributes.txt`.
* `matrix.py`, `benchmark.py` and `summarize.py`: the exact driver, workload
  runner and aggregation/verification script.
* `atime_probe.py`, `atime.json` and `atime-probe.log`: the read-atime check.

Guest inputs and original logs are in `/root/btrfs-bench-20260918/`. The
workload archive also remains at
`/home/mike/obj/btrfs-bench-20260917/src.tar` on the host. The driver contains
the exact formatting commands and rotated schedule and uses the runner's
`--checkpoints` protocol to perform unmounted checks between workloads.
