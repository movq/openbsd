#!/usr/bin/env python3
"""Targeted two-VM SINGLE/DUP member, allocation, and recovery regression.

The host runner reformats the selected scratch disks. See README.
Worker modes run on either guest; inspect runs on Linux with unmounted members.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys

SUPERS = (65536, 67108864)
MIB = 1024 * 1024


def payload(seed, block):
    return hashlib.shake_256(f"{seed}:{block}".encode()).digest(MIB)


def write(path, mib, seed):
    path = Path(path)
    with path.open("wb", buffering=0) as output:
        for block in range(int(mib)):
            data = payload(seed, block)
            assert output.write(data) == len(data)
            if block % 8 == 7:
                os.fsync(output.fileno())
        os.fsync(output.fileno())
    path.with_suffix(".pattern").write_text(json.dumps([int(mib), seed]))
    os.sync()


def verify(root):
    patterns = list(Path(root).rglob("*.pattern"))
    assert patterns, root
    for pattern in patterns:
        mib, seed = json.loads(pattern.read_text())
        with pattern.with_suffix("").open("rb") as data:
            for block in range(mib):
                assert data.read(MIB) == payload(seed, block), (pattern, block)
            assert data.read() == b"", pattern
    compressed = list(Path(root).rglob("compressed"))
    for path in compressed:
        assert path.read_bytes() == b"compressed input\n" * 131072
    print(f"verified {len(patterns)} patterned and {len(compressed)} "
          "compressed files", flush=True)


def inspect(devices):
    """Independently check stripe ownership, accounting, and all DUP copies."""
    def dump(*args):
        return subprocess.check_output(
            ["btrfs", "inspect-internal", *args, devices[0]], text=True)

    members = {}
    generation = None
    for path in devices:
        fd = os.open(path, os.O_RDONLY)
        sb = os.pread(fd, 4096, SUPERS[0])
        devid, size, used = struct.unpack_from("<QQQ", sb, 201)
        assert devid not in members
        members[devid] = (fd, size, used, path)
        nodesize = struct.unpack_from("<I", sb, 148)[0]
        current = struct.unpack_from("<Q", sb, 72)[0]
        generation = current if generation is None else generation
        assert generation == current, "member super generations differ"
        for offset in SUPERS:
            if offset + 4096 > size:
                continue
            mirror = os.pread(fd, 4096, offset)
            assert struct.unpack_from("<Q", mirror, 72)[0] == generation
            assert mirror[201:299] == sb[201:299], "device super items differ"
    chunks, layout = [], []
    usage, data_devices = dict.fromkeys(members, 0), set()
    for item in dump("dump-tree", "-t", "chunk").split("\titem "):
        key = re.search(r"^\d+ key \(\S+ CHUNK_ITEM (\d+)\)", item)
        if key is None:
            continue
        start = int(key[1])
        length = int(re.search(r"\blength (\d+)", item)[1])
        profile = re.search(r"\btype (\S+)", item)[1]
        stripes = [(int(d), int(p)) for d, p in re.findall(
            r"stripe \d+ devid (\d+) offset (\d+)", item)]
        assert profile.endswith(("|single", "|DUP")), profile
        assert len(stripes) == (2 if profile.endswith("|DUP") else 1)
        assert len({d for d, _ in stripes}) == 1, "DUP crosses members"
        for devid, physical in stripes:
            assert physical + length <= members[devid][1]
            usage[devid] += length
            if profile.startswith("DATA|"):
                data_devices.add(devid)
        chunks.append((start, length, stripes))
        layout.append(dict(logical=start, length=length, stripes=stripes,
                           type=profile))
    assert all(usage[d] == members[d][2] for d in members), usage
    faults = []
    extents = re.findall(
        r"^\titem \d+ key \((\d+) (METADATA_ITEM|EXTENT_ITEM) (\d+)\)",
        dump("dump-tree", "-t", "extent"), re.M)
    for logical, kind, length in extents:
        logical = int(logical)
        length = nodesize if kind == "METADATA_ITEM" else int(length)
        start, span, stripes = next(
            c for c in chunks if c[0] <= logical < c[0] + c[1])
        assert logical + length <= start + span
        copies = []
        for devid, physical in stripes:
            physical += logical - start
            for offset in SUPERS:
                assert physical + length <= offset or physical >= offset + 65536
            copies.append(os.pread(members[devid][0], length, physical))
            assert len(copies[-1]) == length
        assert all(c == copies[0] for c in copies), (logical, kind)
        if len(stripes) == 2 and kind == "EXTENT_ITEM" and not faults:
            devid, physical = stripes[0]
            faults.append([devices.index(members[devid][3]),
                           physical + logical - start])
    # Damage the primary chunk-root copy to exercise mount-time DUP fallback.
    chunk_root = struct.unpack_from("<Q", sb, 88)[0]
    start, span, stripes = next(
        c for c in chunks if c[0] <= chunk_root < c[0] + c[1])
    if len(stripes) == 2:
        devid, physical = stripes[0]
        faults.append([devices.index(members[devid][3]),
                       physical + chunk_root - start + 32])
    for fd, _, _, _ in members.values():
        os.close(fd)
    print(json.dumps({"data_devices": sorted(data_devices),
                      "chunks": len(chunks), "faults": faults,
                      "generation": generation, "layout": layout}))


def run(args):
    source = Path(__file__).read_text()
    args.results.mkdir(parents=True, exist_ok=False)
    log = (args.results / "commands.log").open("w", buffering=1)
    linux = [f"/dev/sd{letter}" for letter in "bcde"[:args.members]]
    bsd = [f"/dev/sd{i}c" for i in range(1, args.members + 1)]
    images = [args.images / f"scratch{i}.img"
              for i in range(1, args.members + 1)]
    point = "/mnt/btrfs-multi"
    left, right = "/mnt/btrfs-left", "/mnt/btrfs-right"

    def remote(vm, command, fail=False, code=None):
        log.write(f"{vm}: {command}\n")
        result = subprocess.run(
            ["timeout", str(args.timeout), "ssh", "-o", "ConnectTimeout=10",
             vm, command], input=code, text=True, capture_output=True)
        log.write(result.stdout + result.stderr)
        assert result.returncode not in (124, 255), result.stderr
        assert (result.returncode != 0) == fail, (
            command, result.returncode, result.stdout, result.stderr)
        return result.stdout

    def worker(vm, mode, *values):
        return remote(vm, shlex.join(["python3", "-", mode, *map(str, values)]),
                      code=source)

    def mount(primary=0, ro=False, tree=None, where=point, extra=True, fail=False):
        command = ["mount_btrfs"]
        if ro:
            command += ["-o", "ro"]
        if tree is not None:
            command += ["-s", str(tree)]
        if extra:
            for i, device in enumerate(bsd):
                if i != primary:
                    command += ["-d", device]
        command += [bsd[primary], where]
        remote(args.openbsd, shlex.join(command), fail=fail)

    def unmount(vm, where=point):
        remote(vm, shlex.join(["umount", where]))

    def check():
        remote(args.linux, shlex.join(
            ["btrfs", "check", "--readonly", "--check-data-csum", linux[0]]))
        result = json.loads(worker(args.linux, "inspect", *linux))
        (args.results / "inspection.json").write_text(json.dumps(result, indent=2))
        return result

    # Require both VMs to have no mounted btrfs before reformatting scratch.
    for vm in (args.openbsd, args.linux):
        mounts = remote(vm, "mount")
        assert "type btrfs" not in mounts, mounts
        remote(vm, shlex.join(["mkdir", "-p", point, left, right]))
    mkfs = ["mkfs.btrfs", "-f", "-b", str(256 * MIB), "-d", args.data,
            "-m", args.metadata, "-n", str(args.nodesize), "-O", "^extref",
            "--csum", args.csum, *linux]
    remote(args.linux, shlex.join(mkfs))
    if args.reclaim:
        def phase(name):
            remote(args.openbsd, shlex.join([
                "python3", args.vm_source +
                "/regress/sys/btrfs/reclaim_chunks.py", name,
                point + "/reclaim"]))

        def reassigned(before, after, oldtype, newtype):
            removed = [c for c in before["layout"]
                       if c["type"].startswith(oldtype + "|") and
                       not any(n["logical"] == c["logical"]
                               for n in after["layout"])]
            assert removed, ("no groups returned", oldtype)
            assert any(d == rd and p < rp + old["length"] and
                       rp < p + new["length"]
                       for old in removed for rd, rp in old["stripes"]
                       for new in after["layout"]
                       if new["type"].startswith(newtype + "|")
                       for d, p in new["stripes"]), (
                           "returned physical stripes not reused", newtype)

        mount()
        phase("prepare")
        unmount(args.openbsd)
        before = check()
        assert len(before["data_devices"]) == args.members
        mount(primary=1)
        phase("free_data")
        phase("metadata_bounded")
        unmount(args.openbsd)
        metadata = check()
        reassigned(before, metadata, "DATA", "METADATA")
        mount()
        phase("verify_metadata")
        phase("free_metadata")
        phase("data_again")
        unmount(args.openbsd)
        after = check()
        reassigned(metadata, after, "METADATA", "DATA")
        mount(primary=1, ro=True)
        phase("verify_data")
        unmount(args.openbsd)
        (args.results / "passed").write_text("passed\n")
        print(f"multi-device reclamation passed: {args.results}")
        return
    remote(args.linux, shlex.join(["mount", "-o", "compress=zstd",
                                 linux[0], point]))
    for subvol in ("left", "right"):
        remote(args.linux, f"btrfs subvolume create {point}/{subvol}")
    listing = remote(args.linux, f"btrfs subvolume list {point}")
    ids = {name: int(value) for value, name in re.findall(
        r"ID (\d+).* path (\S+)", listing)}
    imported = 128 if args.data == "dup" else 192
    written = 96 if args.data == "dup" else 80
    worker(args.linux, "write", f"{point}/left/linux", imported, "linux")
    remote(args.linux,
           f"python3 -c \"from pathlib import Path; "
           f"Path('{point}/left/compressed').write_bytes("
           "b'compressed input\\n' * 131072)\"")
    remote(args.linux, f"btrfs subvolume snapshot -r {point}/left {point}/frozen")
    unmount(args.linux)
    check()

    # Failed acquisition must release already-opened members, repeatedly.
    for _ in range(2):
        mount(extra=False, fail=True)
        remote(args.openbsd, f"mount_btrfs -d {bsd[0]} {bsd[0]} {point}",
               fail=True)
        remote(args.openbsd, f"mount_btrfs -d /dev/sd0c {bsd[0]} {point}",
               fail=True)
        remote(args.openbsd, f"mount_btrfs -d /dev/null {bsd[0]} {point}",
               fail=True)
        if args.members == 2:
            remote(args.openbsd,
                   f"mount_btrfs -d /dev/sd3c {bsd[0]} {point}", fail=True)
    # Distinct paths with the same on-disk device identity must also fail.
    saved = []
    with images[0].open("rb") as source_image, \
            images[1].open("r+b", buffering=0) as target:
        for i, offset in enumerate(SUPERS):
            source_image.seek(offset)
            target.seek(offset)
            saved.append(target.read(4096))
            (args.results / f"duplicate-saved-{i}.bin").write_bytes(saved[-1])
            target.seek(offset)
            target.write(source_image.read(4096))
        os.fsync(target.fileno())
    mount(fail=True)
    with images[1].open("r+b", buffering=0) as target:
        for offset, data in zip(SUPERS, saved):
            target.seek(offset)
            target.write(data)
        os.fsync(target.fileno())
    mount(ro=True)
    worker(args.openbsd, "verify", point)
    mount(primary=1, tree=ids["right"], where=right, extra=False, fail=True)
    unmount(args.openbsd)

    # Retain old member supers for interrupted-publication simulation.
    old = []
    with images[0].open("rb") as image:
        for offset in SUPERS:
            image.seek(offset)
            old.append(image.read(4096))
    for i, sb in enumerate(old):
        (args.results / f"old-super-{i}.bin").write_bytes(sb)

    mount(tree=ids["left"], where=left)
    mount(primary=1, tree=ids["right"], where=right, extra=False)
    mount(primary=1, extra=False, fail=True)  # overlapping top-level view
    with ThreadPoolExecutor(2) as pool:
        jobs = [pool.submit(worker, args.openbsd, "write",
                            f"{where}/{name}", written, name)
                for where, name in ((left, "bsd-left"), (right, "bsd-right"))]
        for job in jobs:
            job.result()
    unmount(args.openbsd, left)
    worker(args.openbsd, "write", f"{right}/survivor", 1, "survivor")
    unmount(args.openbsd, right)
    result = check()
    assert len(result["data_devices"]) == args.members, result
    assert result["generation"] > struct.unpack_from("<Q", old[0], 72)[0]

    # Newer trees on another member must win even when mounting via the old one.
    with images[0].open("r+b", buffering=0) as image:
        for offset, sb in zip(SUPERS, old):
            image.seek(offset)
            assert image.write(sb) == len(sb)
        os.fsync(image.fileno())
    for primary in (0, 1):
        mount(primary=primary, ro=True)
        worker(args.openbsd, "verify", point)
        unmount(args.openbsd)
    mount()
    worker(args.openbsd, "write", f"{point}/healed", 1, "healed")
    unmount(args.openbsd)
    result = check()

    # Inject one bad DUP data copy and/or the primary chunk-root copy.
    for number, offset in result["faults"]:
        with images[number].open("r+b", buffering=0) as image:
            image.seek(offset)
            saved = image.read(1)
            (args.results / f"fault-{number}-{offset}.bin").write_bytes(saved)
            image.seek(offset)
            image.write(bytes([saved[0] ^ 0xff]))
            os.fsync(image.fileno())
        mount(ro=True)
        worker(args.openbsd, "verify", point)
        unmount(args.openbsd)
        with images[number].open("r+b", buffering=0) as image:
            image.seek(offset)
            image.write(saved)
            os.fsync(image.fileno())
    check()
    remote(args.linux, shlex.join(["mount", "-o", "ro", linux[-1], point]))
    worker(args.linux, "verify", point)
    unmount(args.linux)
    if args.crash:
        # Reuse the existing DDB driver without running its fixture matrix.
        from run_all import Runner, Serial
        crash_args = argparse.Namespace(
            vm=args.openbsd, image=str(images[0]), device=bsd[0],
            mountpoint=point, vm_source=args.vm_source,
            monitor=args.monitor, timeout=args.timeout, boot_timeout=180)
        harness = Runner(crash_args, args.results)
        harness.log = log
        serial_log = (args.results / "serial.log").open("w", buffering=1)
        harness.serial = Serial(args.serial, serial_log)
        try:
            for name, hits in (("before-super", 1), ("between-members", 3)):
                # The second case stops before the third superblock bwrite:
                # both mirrors of member 0 are written, member 1 is still old.
                mount()
                baseline = check_generation(images)
                harness.crash_break(
                    ["sh", "-c", f"echo {name} > {point}/{name}; sync"],
                    ["btrfs_write_super_mirrors", "bwrite"], hits=hits)
                generations = check_generation(images)
                (args.results / f"{name}-generations.json").write_text(
                    json.dumps(generations))
                if hits == 1:
                    assert generations == baseline
                else:
                    assert generations[0] > baseline[0]
                    assert generations[1:] == baseline[1:]
                harness.boot()
                mount(primary=1, ro=True)
                worker(args.openbsd, "verify", point)
                remote(args.openbsd, f"test -f {point}/{name}", fail=hits == 1)
                unmount(args.openbsd)
                mount(primary=1)
                remote(args.openbsd, f"touch {point}/after-{name}; sync")
                unmount(args.openbsd)
                check()
        finally:
            harness.serial.close()
            serial_log.close()
            if harness.monitor is not None:
                harness.monitor.close()
    (args.results / "passed").write_text("passed\n")
    print(f"multi-device regression passed: {args.results}")


def check_generation(images):
    result = []
    for path in images:
        with path.open("rb") as image:
            image.seek(SUPERS[0] + 72)
            result.append(struct.unpack("<Q", image.read(8))[0])
    return result


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("write", "verify", "inspect"):
        {"write": write, "verify": verify,
         "inspect": lambda *paths: inspect(paths)}[sys.argv[1]](*sys.argv[2:])
    else:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--openbsd", default="root@10.77.0.2")
        parser.add_argument("--linux", default="root@10.77.0.3")
        parser.add_argument("--members", type=int, choices=(2, 4), default=2)
        parser.add_argument("--data", choices=("single", "dup"), default="single")
        parser.add_argument("--metadata", choices=("single", "dup"), default="dup")
        parser.add_argument("--nodesize", type=int, default=16384)
        parser.add_argument("--csum", choices=("crc32c", "xxhash"), default="crc32c")
        parser.add_argument("--timeout", type=int, default=600)
        parser.add_argument("--crash", action="store_true",
                            help="also reset at DDB superblock write boundaries")
        parser.add_argument("--reclaim", action="store_true",
                            help="run the member space reclamation test instead")
        parser.add_argument("--vm-source", default="/mnt/src")
        parser.add_argument("--serial", default="/tmp/serial.sock")
        parser.add_argument("--monitor", default="/tmp/monitor.sock")
        parser.add_argument("--images", type=Path, default=Path("/home/mike/obj"))
        parser.add_argument("--results", type=Path, default=Path(
            "/home/mike/obj/btrfs-multi-" +
            datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
        options = parser.parse_args()
        if options.reclaim and (options.members != 2 or options.crash):
            parser.error("--reclaim requires two members and excludes --crash")
        run(options)
