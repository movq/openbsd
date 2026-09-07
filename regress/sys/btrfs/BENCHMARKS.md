# Btrfs / FFS2 performance

Measured with OpenBSD GENERIC.MP#135, containing the range-read, buffer,
allocation-pool and asynchronous-write improvements described in
[PROFILING.md](PROFILING.md). The guest has eight vCPUs, 1 GiB RAM,
`kern.bufcachepercent=20`, and virtio-scsi disks backed by host-cached sparse
images. Tracing was disabled, and no workload overlapped a kernel build.

Times are elapsed seconds; smaller is better. Btrfs sequential values are two
separate passes on fresh filesystems; each tree workload has one sample.
FFS2 values are control passes before and after btrfs. Relative throughput
is FFS2 time divided by btrfs time, spanning the measurements; higher is better.

| Workload | FFS2 seconds, before / after | Btrfs seconds | Btrfs / FFS2 throughput |
| --- | ---: | ---: | ---: |
| Extract source tree | 13.632 / 11.214 | 16.048 | 0.70–0.85x |
| Recursive grep after remount | 11.789 / 11.447 | 10.889 | 1.05–1.08x |
| Immediate grep repeat | 11.909 / 11.809 | 11.394 | 1.04–1.05x |
| Recursive chown | 0.948 / 0.904 | 1.748 | 0.52–0.54x |
| Recursive chmod | 0.901 / 0.897 | 1.980 | 0.45–0.46x |
| Delete source tree | 4.039 / 3.981 | 10.939 | 0.36–0.37x |
| Write 2 GiB | 3.593 / 3.552 | 10.509 / 10.820 | 0.33–0.34x |
| Read 2 GiB after remount | 2.385 / 2.364 | 1.799 / 1.853 | 1.28–1.33x |
| Immediate read repeat | 2.593 / 2.432 | 1.769 / 1.899 | 1.28–1.47x |

Reads, extraction, grep and chown meet a target of half FFS2 throughput in
these workloads. Chmod is borderline; bulk writes and deletion remain the
largest gaps. These VM measurements are workload-specific, and the FFS2
extraction variation warrants caution about small differences.

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
`/home/mike/obj/btrfs-architecture-profile`, using `experiment-retained-*`
and `experiment-ffs-*` prefixes. The host runner is
`/home/mike/obj/btrfs-architecture-profile.py`. Architectural findings,
implementation comparisons and targeted correctness coverage are recorded
in [PROFILING.md](PROFILING.md).
