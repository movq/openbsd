# Btrfs / FFS2 performance

Measured with OpenBSD GENERIC.MP#140, containing the range-read, buffer,
allocation-pool, asynchronous-write and write-batching improvements described in
[PROFILING.md](PROFILING.md). The guest has eight vCPUs, 1 GiB RAM,
`kern.bufcachepercent=20`, and virtio-scsi disks backed by host-cached sparse
images. Tracing was disabled, and no workload overlapped a kernel build.

Times are elapsed seconds; smaller is better. Btrfs sequential values are two
separate passes on fresh filesystems; each tree workload has one sample.
FFS2 values are control passes before and after btrfs. Relative throughput
is FFS2 time divided by btrfs time, spanning the measurements; higher is better.

| Workload | FFS2 seconds, before / after | Btrfs seconds | Btrfs / FFS2 throughput |
| --- | ---: | ---: | ---: |
| Extract source tree | 9.311 / 13.355 | 15.137 | 0.62–0.88x |
| Recursive grep after remount | 9.910 / 11.684 | 11.359 | 0.87–1.03x |
| Immediate grep repeat | 10.781 / 11.960 | 11.768 | 0.92–1.02x |
| Recursive chown | 0.912 / 0.929 | 1.795 | 0.51–0.52x |
| Recursive chmod | 0.791 / 0.869 | 1.958 | 0.40–0.44x |
| Delete source tree | 3.755 / 4.070 | 10.952 | 0.34–0.37x |
| Write 2 GiB | 2.959 / 3.561 | 9.238 / 9.189 | 0.32–0.39x |
| Read 2 GiB after remount | 1.894 / 2.407 | 1.923 / 1.916 | 0.98–1.26x |
| Immediate read repeat | 1.915 / 2.501 | 2.114 / 2.054 | 0.91–1.22x |

Reads, extraction, grep and chown meet a target of half FFS2 throughput in
these workloads. Chmod, bulk writes and deletion remain below that target.
These VM measurements are workload-specific. Variation between the FFS2
controls, especially extraction and sequential I/O, limits precision;
the ranges span observed values and are not confidence intervals.

## Workloads and method

The uncompressed source archive contains 86,160 files and 7,261 directories,
totaling 1.438 GiB of file data. The runner extracts with `tar -xpf`, scans
contents with `grep -r -a -F -c Copyright`, changes ownership with
`chown -R 12345:12345` and modes with `chmod -R u=rwX,go=rX`, then deletes
the populated tree with `rm -rf`. Sequential passes write and read 2 GiB
with `dd bs=1m`.

Each filesystem is freshly formatted on a 100 GiB scratch disk and mounted
with `noatime`. Btrfs uses 16 KiB nodes, 4 KiB sectors, SINGLE data, DUP
metadata, extent-format free-space records, NO_HOLES, and no separate
block-group tree. Data is uncompressed and checksummed. FFS2 uses 16 KiB
blocks and 2 KiB fragments.

Mutation times include the following unmount to account for writeback.
Mounting, formatting, and verification are excluded. Each workload starts
after remount except immediate read repeats. Deletion uses the populated
tree after ownership/mode changes. Remount clears filesystem caches but
does not clear host caches. The 2 GiB file exceeds guest RAM, so the immediate
repeat is not a fully memory-resident workload.

All btrfs passes passed unmounted `btrfs check --readonly --check-data-csum`
and metadata-mirror checks. Sequential files were compared completely with
zeroes after read-only remount, and deletion was verified after remount.
FFS2 passes passed `fsck_ffs -fn`.

Raw timings and check logs are in
`/home/mike/obj/btrfs-architecture-profile`, using `write-final-*`
prefixes, including `write-final-ffs-before*` and `write-final-ffs-after*`
for the controls. The host runner is
`/home/mike/obj/btrfs-architecture-profile.py`. Architectural findings,
implementation comparisons and targeted correctness coverage are recorded
in [PROFILING.md](PROFILING.md).
