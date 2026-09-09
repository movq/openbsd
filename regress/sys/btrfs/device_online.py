#!/usr/bin/env python3
"""Destructive, targeted online device administration test on four scratch disks.

Uses OpenBSD sd1a..sd4a and matching Linux loops at offset 1 MiB. Leaves all
filesystems unmounted on success; failure preserves mounts for diagnosis.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime
import json
import os
from pathlib import Path
import shlex
import subprocess

from multi_device import MIB
import multi_device


def run(args):
    args.results.mkdir(parents=True, exist_ok=False)
    log = (args.results / "commands.log").open("w", buffering=1)
    point = "/mnt/btrfs-device"
    sizes = [512, 1536, 32, 2048]
    bsd = [f"/dev/sd{i}a" for i in range(1, 5)]
    loops = []
    worker = Path(multi_device.__file__).read_text()

    def remote(vm, command, fail=False, code=None):
        log.write(f"{vm}: {command}\n")
        result = subprocess.run(
            ["timeout", str(args.timeout), "ssh", "-o", "ConnectTimeout=10",
             vm, command], input=code, text=True, capture_output=True)
        log.write(result.stdout + result.stderr)
        assert result.returncode not in (124, 255), result.stderr
        assert bool(result.returncode) == fail, (
            command, result.returncode, result.stdout, result.stderr)
        return result.stdout

    def b(command, **kw):
        return remote(args.openbsd, command, **kw)

    def l(command, **kw):
        return remote(args.linux, command, **kw)

    def work(vm, mode, *values):
        return remote(vm, shlex.join(["python3", "-", mode, *map(str, values)]),
                      code=worker)

    def mount(members, ro=False):
        b(shlex.join(["mount_btrfs", *(["-o", "ro"] if ro else []),
                      *[v for i in members[1:] for v in ("-d", bsd[i])],
                      bsd[members[0]], point]))

    def check(members, name):
        paths = [loops[i] for i in members]
        # Linux loop backing-device caches otherwise retain the previous guest's
        # bytes even after the filesystem itself has been unmounted.
        for path in [f"/dev/sd{x}" for x in "bcde"] + loops:
            l(f"blockdev --flushbufs {path}")
        l(shlex.join(["btrfs", "device", "scan", *paths]))
        l(shlex.join(["btrfs", "check", "--readonly", "--check-data-csum",
                      paths[0]]))
        result = json.loads(work(args.linux, "inspect", *paths))
        (args.results / f"{name}.json").write_text(json.dumps(result, indent=2))
        l(shlex.join(["mount", "-o", "ro", paths[0], point]))
        work(args.linux, "verify", point)
        l(f"sha256sum -c /root/btrfs-device.sha")
        l("python3 -", code=f"""
import os
root = {point!r}
for prefix in ("base", "snap/base"):
    a, b = root + "/" + prefix + "/imported", root + "/" + prefix + "/hardlink"
    assert os.stat(a).st_ino == os.stat(b).st_ino
    assert os.getxattr(a, "user.device-test") == b"preserved"
""")
        l(f"umount {point}")
        return result

    for vm in (args.openbsd, args.linux):
        assert "type btrfs" not in remote(vm, "mount")
        remote(vm, f"mkdir -p {point}")
    # Label sectors lie outside the tested partitions and survive device add -f.
    for i, size in enumerate(sizes, 1):
        b("python3 -", code=f"""
import pathlib, subprocess
label = subprocess.check_output(["disklabel", "sd{i}"], text=True)
label = "\\n".join(line for line in label.splitlines()
                  if not line.lstrip().startswith("a:"))
label += "\\n  a: {size * 2048} 2048 4.2BSD 2048 16384 1\\n"
pathlib.Path("/tmp/btrfs-device.label").write_text(label)
subprocess.run(["disklabel", "-R", "sd{i}", "/tmp/btrfs-device.label"], check=True)
""")
        loop = l(f"losetup --find --show --offset {MIB} "
                 f"--sizelimit {size * MIB} /dev/sd{'bcde'[i - 1]}").strip()
        loops.append(loop)
        l("python3 - " + loop, code="""
import os, sys
fd = os.open(sys.argv[1], os.O_RDWR)
os.pwrite(fd, bytes(1024 * 1024), 0)
if os.lseek(fd, 0, os.SEEK_END) >= 67112960:
    os.pwrite(fd, bytes(4096), 67108864)
os.fsync(fd)
os.close(fd)
""")
    l(shlex.join(["mkfs.btrfs", "-f", "-d", args.data, "-m", args.metadata,
                  "-n", str(args.nodesize), "--csum", args.csum, loops[0]]))
    l(f"mount -o compress=zstd {loops[0]} {point} && mkdir {point}/base")
    work(args.linux, "write", point + "/base/imported", 48, "imported")
    l(f"mkdir {point}/nocow && chattr +C {point}/nocow && "
      f"dd if=/dev/urandom of={point}/nocow/data bs=1M count=2 status=none && "
      f"fallocate -l 8M {point}/prealloc && "
      f"dd if=/dev/urandom of={point}/prealloc bs=4096 count=3 seek=9 "
      f"conv=notrunc status=none && "
      f"ln {point}/base/imported {point}/base/hardlink && "
      f"cp --reflink=always {point}/base/imported {point}/clone")
    l("python3 -", code=f"""
from pathlib import Path
import os
root = Path({point!r})
(root / "base/compressed").write_bytes(b"compressed input\\n" * 131072)
os.setxattr(root / "base/imported", "user.device-test", b"preserved")
""")
    l(f"btrfs subvolume snapshot -r {point} {point}/snap")
    l(f"sha256sum {point}/nocow/data {point}/prealloc {point}/clone "
      f"> /root/btrfs-device.sha && umount {point}")
    l("python3 - " + loops[3], code="""
import os, sys
fd = os.open(sys.argv[1], os.O_RDWR)
os.pwrite(fd, b"existing device signature", 4096)
os.fsync(fd)
os.close(fd)
""")
    if args.faults:
        layout = json.loads(work(args.linux, "inspect", loops[0]))
        ranges = json.loads(l("python3 - " + loops[0], code="""
import json, re, subprocess, sys
text = subprocess.check_output(
    ["btrfs", "inspect-internal", "dump-tree", "-t", "extent", sys.argv[1]],
    text=True)
print(json.dumps([(int(a), int(b)) for a, b in re.findall(
    r"key \\((\\d+) EXTENT_ITEM (\\d+)\\)", text) if int(b) >= 8192]))
"""))
        logical, length = ranges[0]
        chunk = next(c for c in layout["layout"]
                     if c["logical"] <= logical <
                     c["logical"] + c["length"])
        assert len(chunk["stripes"]) == 2
        offsets = [p + logical - chunk["logical"] +
                   (0 if args.bad_mirrors else i * 4096)
                   for i, (_, p) in enumerate(chunk["stripes"])]
        offsets += [layout["faults"][-1][1]]  # primary bootstrap metadata
        # Shared raw backing files are only changed while both VMs are unmounted.
        with Path("/home/mike/obj/scratch1.img").open("r+b", buffering=0) as image:
            for n, offset in enumerate(offsets):
                image.seek(MIB + offset)
                old = image.read(1)
                (args.results / f"fault-{n}-{offset}.bin").write_bytes(old)
                image.seek(MIB + offset)
                image.write(bytes([old[0] ^ 0xff]))
            os.fsync(image.fileno())

    mount([0], ro=True)
    b(f"btrfs device add {bsd[1]} {point}", fail=True)
    b(f"umount {point}")
    mount([0])
    b(f"btrfs device remove 1 {point}", fail=True)
    for bad in ("/dev/sd0a", bsd[0], "/dev/null", "/tmp/btrfs-device.label"):
        b(f"btrfs device add -f {bad} {point}", fail=True)
    b(f"btrfs device remove 999 {point}", fail=True)
    b(f"btrfs device remove /dev/sd4c {point}", fail=True)
    b(f"btrfs device add {bsd[3]} {point}", fail=True)
    b(f"btrfs device add -f {bsd[3]} {point}")
    b(f"btrfs device remove {bsd[3]} {point}")
    if args.faults:
        b(f"btrfs device add {bsd[1]} {point}")
        b(f"btrfs device remove {bsd[0]} {point}", fail=args.bad_mirrors)
        if args.bad_mirrors:
            b(f"touch {point}/must-fail", fail=True)
        else:
            work(args.openbsd, "verify", point)
        b(f"umount {point}")
        if args.bad_mirrors:
            with Path("/home/mike/obj/scratch1.img").open(
                    "r+b", buffering=0) as image:
                for n, offset in enumerate(offsets):
                    image.seek(MIB + offset)
                    image.write((args.results / f"fault-{n}-{offset}.bin").
                                read_bytes())
                os.fsync(image.fileno())
            mount([0, 1], ro=True)
            work(args.openbsd, "verify", point)
            b(f"umount {point}")
            check([0, 1], "aborted-copy")
        else:
            check([1], "repaired-copies")
        for loop in loops:
            l(f"losetup -d {loop}")
        (args.results / "passed").write_text("passed\n")
        print(f"device mirror regression passed: {args.results}")
        return
    if args.crash:
        from run_all import Runner, Serial
        crash_args = argparse.Namespace(
            vm=args.openbsd, image="/home/mike/obj/scratch1.img",
            device=bsd[0], mountpoint=point, vm_source="/mnt/src",
            monitor=args.monitor, timeout=args.timeout, boot_timeout=180)
        harness = Runner(crash_args, args.results)
        harness.log = log
        serial_log = (args.results / "serial.log").open("w", buffering=1)
        harness.serial = Serial(args.serial, serial_log)

        def crash(command, targets, hits=1):
            harness.crash_break(shlex.split(command), targets, hits=hits)
            harness.boot()

        try:
            crash(f"btrfs device add {bsd[1]} {point}",
                  ["btrfs_write_super_mirrors"])
            mount([0])
            work(args.openbsd, "verify", point)
            # New member's first super is published before any old member.
            crash(f"btrfs device add {bsd[1]} {point}",
                  ["btrfs_write_super_mirrors", "bwrite"], hits=2)
            mount([0, 1])
            work(args.openbsd, "verify", point)
            b(f"echo healed > {point}/healed; sync; umount {point}")
            check([0, 1], "crash-add")
            mount([0, 1])
            crash(f"btrfs device remove {bsd[0]} {point}",
                  ["btrfs_chunk_copy"])
            mount([0, 1])
            work(args.openbsd, "verify", point)
            # Destination bytes are durable but supers disagree on the map.
            crash(f"btrfs device remove {bsd[0]} {point}",
                  ["btrfs_write_super_mirrors", "bwrite"], hits=2)
            mount([1, 0])
            work(args.openbsd, "verify", point)
            b(f"echo moved > {point}/moved; sync; umount {point}")
            check([0, 1], "crash-move")
            mount([0, 1])
            # Final membership is durable; the old member still has its magic.
            crash(f"btrfs device remove {bsd[0]} {point}",
                  ["btrfs_erase_member"])
            mount([1])
            work(args.openbsd, "verify", point)
            b(f"echo detached > {point}/detached; sync; umount {point}")
            check([1], "crash-remove")
        finally:
            harness.serial.close()
            serial_log.close()
            if harness.monitor is not None:
                harness.monitor.close()
        for loop in loops:
            l(f"losetup -d {loop}")
        (args.results / "passed").write_text("passed\n")
        print(f"device crash regression passed: {args.results}")
        return
    # Insufficient destination space must leave the member attached and usable,
    # including when a prefix of its chunks has already moved.
    b(f"btrfs device add {bsd[2]} {point}")
    b(f"btrfs device remove 1 {point}", fail=True)
    work(args.openbsd, "verify", point)
    b(f"echo survived > {point}/enospc && sync && umount {point}")
    check([0, 2], "enospc")
    mount([0, 2])
    b(f"btrfs device add {bsd[1]} {point}")
    b(f"btrfs device add -f {bsd[1]} {point}", fail=True)
    # Immediately remove an empty member, and add it again without forcing.
    b(f"btrfs device remove {bsd[1]} {point}")
    b(f"btrfs device add {bsd[1]} {point}")
    b(f"btrfs device remove {bsd[2]} {point}")
    b(f"btrfs device add {bsd[2]} {point}")
    work(args.openbsd, "write", point + "/pooled", 160, "pooled")
    b(f"umount {point}")
    before = check([0, 1, 2], "pooled")
    assert len(before["data_devices"]) >= 2, before

    # The primary device owns bootstrap/system chunks and shared metadata.
    # Keep read and write traffic active while evacuating it.
    mount([0, 1, 2])
    with ThreadPoolExecutor(3) as pool:
        jobs = [
            pool.submit(work, args.openbsd, "verify", point),
            pool.submit(work, args.openbsd, "write", point + "/concurrent",
                        12, "concurrent"),
            pool.submit(b, f"btrfs device remove {bsd[0]} {point}"),
        ]
        for job in jobs:
            job.result()
    work(args.openbsd, "verify", point)
    b(f"btrfs device remove {bsd[0]} {point}", fail=True)
    b("sync")
    statcode = f"import os; s=os.statvfs({point!r}); print(s.f_blocks,s.f_bfree)"
    live_space = b("python3 -", code=statcode)
    b(f"umount {point}")
    mount([1, 2])
    assert b("python3 -", code=statcode) == live_space
    b(f"umount {point}")
    after = check([1, 2], "removed-primary")
    original = {c["logical"]: c for c in before["layout"]}
    for c in after["layout"]:
        if c["logical"] in original:
            assert c["length"] == original[c["logical"]]["length"]
            assert c["type"] == original[c["logical"]]["type"]
    mount([2, 1])
    # Add a differently sized member and remove a populated secondary by ID.
    b(f"btrfs device add {bsd[3]} {point}")
    b(f"btrfs device remove 3 {point}")
    work(args.openbsd, "verify", point)
    b(f"umount {point}")
    check([2, 3], "removed-secondary")
    # Reuse an erased member; all four device objects have changed ownership.
    mount([3, 2])
    b(f"btrfs device add {bsd[0]} {point}")
    work(args.openbsd, "write", point + "/reuse", 4, "reuse")
    b(f"umount {point}")
    check([0, 2, 3], "reused")
    for loop in loops:
        l(f"losetup -d {loop}")
    (args.results / "passed").write_text("passed\n")
    print(f"online device regression passed: {args.results}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openbsd", default="root@10.77.0.2")
    parser.add_argument("--linux", default="root@10.77.0.3")
    parser.add_argument("--data", choices=("single", "dup"), default="single")
    parser.add_argument("--metadata", choices=("single", "dup"), default="dup")
    parser.add_argument("--nodesize", type=int, default=16384)
    parser.add_argument("--csum", choices=("crc32c", "xxhash"), default="crc32c")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--crash", action="store_true",
                        help="run publication-boundary resets instead")
    parser.add_argument("--faults", action="store_true",
                        help="test relocation with corrupt DUP copies instead")
    parser.add_argument("--bad-mirrors", action="store_true",
                        help="with --faults, corrupt both copies of one sector")
    parser.add_argument("--serial", default="/tmp/serial.sock")
    parser.add_argument("--monitor", default="/tmp/monitor.sock")
    parser.add_argument("--results", type=Path, default=Path(
        "/home/mike/obj/btrfs-device-" +
        datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    options = parser.parse_args()
    if options.faults and (options.crash or options.data != "dup" or
                          options.metadata != "dup"):
        parser.error("--faults requires DUP data and metadata, without --crash")
    if options.bad_mirrors and not options.faults:
        parser.error("--bad-mirrors requires --faults")
    run(options)
