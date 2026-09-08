#!/usr/bin/env python3
"""Targeted Linux/OpenBSD tree-log interoperability and crash regression.

Reformats scratch1.img (/dev/sd1c and /dev/sdb). Both VMs must have every
btrfs filesystem unmounted. The matching OpenBSD kernel must be installed.
Only one guest mounts the filesystem at a time. A stopped writer is reset
without unmounting, and each crash image is replayed independently by both
kernels. See README for invocation and limitations.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import sys
import time

MIB = 1024 * 1024


def data(tag, size=256 * 1024):
    return hashlib.shake_256(tag.encode()).digest(size)


def fsync(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def seed(root, case):
    root = Path(root)
    for name in ("file", "second", "unsynced", "source", "victim"):
        (root / name).write_bytes(data(name))
    (root / "dir").mkdir()
    (root / "dir" / "removed").write_bytes(data("removed"))
    (root / "dir" / "kept").write_bytes(data("kept"))
    if hasattr(os, "setxattr"):
        os.setxattr(root / "file", "user.remove", b"old")
        os.setxattr(root / "file", "user.keep", b"keep")
    if case == "subvol":
        for name in ("left", "right"):
            subprocess.run(["btrfs", "subvolume", "create", str(root / name)],
                           check=True)
            (root / name / "file").write_bytes(data(name))
    os.sync()


def mutate(root, case):
    root = Path(root)
    if sys.platform.startswith("linux"):
        # Finish Linux's first-write chunk allocation before testing a log.
        (root / "unsynced").write_bytes(data("unsynced"))
        (root / "warmup").mkdir()
        (root / "warmup" / "child").write_bytes(data("warmup", 17001))
        fsync(root / "warmup" / "child")
        os.sync()
    if case == "data":
        for name, offset, tag in (("file", 4096, "first"),
                                  ("second", 8192, "second-change"),
                                  ("file", 256 * 1024, "append")):
            fd = os.open(root / name, os.O_RDWR)
            assert os.pwrite(fd, data(tag, 8192), offset) == 8192
            os.fsync(fd)
            os.close(fd)
    elif case == "truncate":
        os.truncate(root / "file", 17001)
        fsync(root / "file")
    elif case == "namespace":
        os.unlink(root / "dir" / "removed")
        os.rename(root / "source", root / "victim")
        os.link(root / "file", root / "hardlink")
        (root / "newdir").mkdir()
        (root / "newdir" / "child").write_bytes(data("child", 17001))
        os.symlink("../file", root / "newdir" / "symlink")
        os.removexattr(root / "file", "user.remove")
        os.setxattr(root / "file", "user.add", b"new")
        for name in ("file", "victim", "newdir/child", "newdir", "dir"):
            fsync(root / name)
        fsync(root)
    elif case == "create":
        (root / "newdir").mkdir()
        (root / "newdir" / "child").write_bytes(data("child", 17001))
        fsync(root / "newdir" / "child")
    elif case == "unlink":
        os.unlink(root / "dir" / "removed")
        fsync(root / "dir")
    elif case == "rename":
        os.rename(root / "source", root / "renamed")
        fsync(root / "renamed")
    elif case == "xattr":
        os.removexattr(root / "file", "user.remove")
        os.setxattr(root / "file", "user.add", b"new")
        fsync(root / "file")
    elif case == "hardlink":
        os.link(root / "file", root / "hardlink")
        fsync(root / "file")
    elif case == "extref":
        for i in range(48):
            os.link(root / "file", root / ("link-%03d-" % i + "x" * 80))
        os.sync()
        os.chmod(root / "file", 0o600)
        fsync(root / "file")
    elif case == "replace":
        os.rename(root / "source", root / "victim")
        fsync(root / "victim")
        fsync(root)
    elif case == "crossdir":
        os.rename(root / "source", root / "dir" / "moved")
        fsync(root / "dir" / "moved")
        fsync(root)
    elif case == "sparse":
        os.truncate(root / "file", 4096)
        fd = os.open(root / "file", os.O_RDWR)
        os.pwrite(fd, data("tail", 17001), 1024 * 1024)
        os.fsync(fd)
        os.close(fd)
    elif case == "shared":
        subprocess.run(["cp", "--reflink=always", str(root / "file"),
                        str(root / "second")], check=True)
        fsync(root / "second")
    elif case == "compressed":
        subprocess.run(["btrfs", "property", "set", str(root / "file"),
                        "compression", "zstd"], check=True)
        (root / "file").write_bytes(b"compressed log data\n" * 20000)
        fsync(root / "file")
    elif case == "prealloc":
        fd = os.open(root / "file", os.O_RDWR)
        os.posix_fallocate(fd, 256 * 1024, 256 * 1024)
        os.pwrite(fd, data("allocated", 4096), 300 * 1024)
        os.fsync(fd)
        os.close(fd)
    elif case == "symlink":
        os.symlink("file", root / "symlink")
        fsync(root)
    elif case == "subvol":
        for name in ("left", "right"):
            fd = os.open(root / name / "file", os.O_RDWR)
            os.pwrite(fd, data(name + "-change", 8192), 4096)
            os.fsync(fd)
            os.close(fd)
    elif case == "overwrite":
        fd = os.open(root / "file", os.O_RDWR)
        os.pwrite(fd, data("first", 8192), 4096)
        os.fsync(fd)
        os.pwrite(fd, data("last", 4096), 8192)
        os.fsync(fd)
        os.close(fd)
    elif case == "resize":
        fd = os.open(root / "file", os.O_RDWR)
        os.pwrite(fd, data("first", 8192), 4096)
        os.fsync(fd)
        os.ftruncate(fd, 10001)
        os.fsync(fd)
        os.close(fd)
    elif case in ("unsynced", "fallback"):
        # Exercise an already tracked inode, then edit mappings and namespace.
        # Either crash with the old publication or force a full fsync commit.
        assert not sys.platform.startswith("linux")
        fd = os.open(root / "file", os.O_RDWR)
        os.pwrite(fd, data("first", 8192), 4096)
        os.fsync(fd)
        os.fsync(fd)
        os.pwrite(fd, data("unpublished", 8192), 256 * 1024)
        os.close(fd)
        os.rename(root / "file", root / "renamed")
        if case == "fallback":
            fsync(root / "renamed")
    elif case == "concurrent":
        from concurrent.futures import ThreadPoolExecutor

        def write(name):
            fd = os.open(root / name, os.O_RDWR)
            for i in range(8):
                os.pwrite(fd, data(name + str(i), 4096), i * 4096)
                os.fsync(fd)
            os.close(fd)

        with ThreadPoolExecutor(2) as pool:
            list(pool.map(write, ("file", "second")))
    else:
        raise ValueError(case)
    print("FSYNC_COMPLETE", flush=True)


def verify(root, case):
    root = Path(root)
    file_path = root / "file"
    expected = bytearray(data("file"))
    second = bytearray(data("second"))
    if case == "data":
        expected[4096:12288] = data("first", 8192)
        expected.extend(data("append", 8192))
        second[8192:16384] = data("second-change", 8192)
    elif case == "truncate":
        expected = expected[:17001]
    elif case == "namespace":
        assert not (root / "source").exists()
        assert not (root / "dir" / "removed").exists()
        assert (root / "dir" / "kept").read_bytes() == data("kept")
        assert (root / "victim").read_bytes() == data("source")
        assert (root / "newdir" / "child").read_bytes() == data("child", 17001)
        assert os.readlink(root / "newdir" / "symlink") == "../file"
        assert (root / "file").stat().st_ino == (root / "hardlink").stat().st_ino
        assert (root / "file").stat().st_nlink == 2
        if hasattr(os, "getxattr"):
            assert os.getxattr(root / "file", "user.keep") == b"keep"
            assert os.getxattr(root / "file", "user.add") == b"new"
            assert "user.remove" not in os.listxattr(root / "file")
    elif case == "create":
        assert (root / "newdir" / "child").read_bytes() == data("child", 17001)
    elif case == "unlink":
        assert not (root / "dir" / "removed").exists()
        assert (root / "dir" / "kept").read_bytes() == data("kept")
    elif case == "rename":
        assert not (root / "source").exists()
        assert (root / "renamed").read_bytes() == data("source")
    elif case == "xattr" and hasattr(os, "getxattr"):
        assert os.getxattr(root / "file", "user.keep") == b"keep"
        assert os.getxattr(root / "file", "user.add") == b"new"
        assert "user.remove" not in os.listxattr(root / "file")
    elif case == "hardlink":
        assert (root / "file").stat().st_ino == (root / "hardlink").stat().st_ino
        assert (root / "file").stat().st_nlink == 2
    elif case == "extref":
        assert (root / "file").stat().st_nlink == 49
        assert (root / "file").stat().st_mode & 0o777 == 0o600
        for i in range(48):
            assert (root / ("link-%03d-" % i + "x" * 80)).stat().st_ino == (
                root / "file").stat().st_ino
    elif case == "replace":
        assert not (root / "source").exists()
        assert (root / "victim").read_bytes() == data("source")
    elif case == "crossdir":
        assert not (root / "source").exists()
        assert (root / "dir" / "moved").read_bytes() == data("source")
    elif case == "sparse":
        expected = expected[:4096] + bytes(1024 * 1024 - 4096)
        expected += data("tail", 17001)
    elif case == "shared":
        second = expected
    elif case == "compressed":
        expected = b"compressed log data\n" * 20000
    elif case == "prealloc":
        expected.extend(bytes(256 * 1024))
        expected[300 * 1024:304 * 1024] = data("allocated", 4096)
    elif case == "symlink":
        assert os.readlink(root / "symlink") == "file"
    elif case == "subvol":
        for name in ("left", "right"):
            content = bytearray(data(name))
            content[4096:12288] = data(name + "-change", 8192)
            assert (root / name / "file").read_bytes() == content
    elif case in ("overwrite", "resize"):
        expected[4096:12288] = data("first", 8192)
        if case == "overwrite":
            expected[8192:12288] = data("last", 4096)
        else:
            expected = expected[:10001]
    elif case == "concurrent":
        for name, content in (("file", expected), ("second", second)):
            for i in range(8):
                content[i * 4096:(i + 1) * 4096] = data(name + str(i), 4096)
    elif case == "unsynced":
        expected[4096:12288] = data("first", 8192)
        assert not (root / "renamed").exists()
    elif case == "fallback":
        expected[4096:12288] = data("first", 8192)
        expected.extend(data("unpublished", 8192))
        assert not file_path.exists()
        file_path = root / "renamed"
    assert file_path.read_bytes() == expected
    assert (root / "second").read_bytes() == second
    print(f"verified {case}", flush=True)


def command(args, timeout=180):
    print("+", shlex.join(map(str, args)), flush=True)
    result = subprocess.run(args, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    return result.stdout


def ssh(guest, cmd, timeout=180):
    return command(["timeout", str(timeout), "ssh", "-o", "ConnectTimeout=5",
                    f"root@10.77.0.{2 if guest == 'openbsd' else 3}", cmd],
                   timeout + 10)


def monitor(guest, cmd):
    path = "/tmp/monitor.sock" if guest == "openbsd" else "/tmp/monitor-alpine.sock"
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        sock.connect(path)
        sock.recv(65536)
        sock.sendall((cmd + "\n").encode())
        output = b""
        while b"(qemu)" not in output:
            output += sock.recv(65536)
        print(output.decode(errors="replace"), flush=True)


def boot(guest):
    monitor(guest, "system_reset")
    monitor(guest, "cont")
    time.sleep(3)
    end = time.monotonic() + 120
    while time.monotonic() < end:
        try:
            ssh(guest, "true", 5)
            return
        except (subprocess.SubprocessError, OSError):
            time.sleep(2)
    raise TimeoutError(f"{guest} did not boot")


def super_info(image):
    with open(image, "rb", buffering=0) as disk:
        disk.seek(65536)
        sb = disk.read(4096)
    return dict(generation=struct.unpack_from("<Q", sb, 72)[0],
                log_root=struct.unpack_from("<Q", sb, 96)[0],
                log_level=sb[200])


def mount(guest, readonly=False):
    option = "-o ro " if readonly else ""
    if guest == "linux":
        ssh(guest, "mkdir -p /mnt/log; blockdev --flushbufs /dev/sdb; "
            f"mount -t btrfs {option}-o commit=3600 /dev/sdb /mnt/log")
    else:
        ssh(guest, f"mkdir -p /mnt/log; mount_btrfs {option}/dev/sd1c /mnt/log")


def worker(guest, phase, case):
    command(["scp", __file__,
             f"root@10.77.0.{2 if guest == 'openbsd' else 3}:/tmp/log_tree.py"])
    ssh(guest, f"python3 /tmp/log_tree.py {phase} /mnt/log {case}")


def faults(args):
    """Damage both DUP log-root copies in a saved, CRC32C crash fixture."""
    from chunks_fixture import checksum
    from reclaim_chunks import chunks

    image = Path("/home/mike/obj/scratch1.img")
    snapshot = Path(args.fault_image)
    for guest in ("openbsd", "linux"):
        assert " type btrfs " not in ssh(guest, "mount")
    for kind in ("checksum", "generation", "cycle", "empty"):
        command(["dd", f"if={snapshot}", f"of={image}", "bs=1M",
                 "count=1024", "conv=notrunc,fsync", "status=none"])
        logical = super_info(image)["log_root"]
        assert logical
        chunk = next(c for c in chunks(image)
                     if c["logical"] <= logical < c["logical"] + c["length"])
        with image.open("r+b", buffering=0) as disk:
            disk.seek(65536)
            superblock = disk.read(4096)
            nodesize = struct.unpack_from("<I", superblock, 148)[0]
            assert struct.unpack_from("<H", superblock, 196)[0] == 0
            for stripe in chunk["physical"]:
                address = stripe + logical - chunk["logical"]
                disk.seek(address)
                block = bytearray(disk.read(nodesize))
                assert block[100] == 0
                if kind == "checksum":
                    block[0] ^= 255
                else:
                    if kind == "generation":
                        struct.pack_into("<Q", block, 80, 1)
                    elif kind == "cycle":
                        offset = struct.unpack_from("<I", block, 101 + 17)[0]
                        # root_item.bytenr follows inode, generation, root_dirid.
                        struct.pack_into("<Q", block, 101 + offset + 176, logical)
                    else:
                        struct.pack_into("<I", block, 96, 0)
                    checksum(block)
                disk.seek(address)
                disk.write(block)
            os.fsync(disk.fileno())
        if kind == "empty":
            mount("openbsd")
            worker("openbsd", "verify", "unchanged")
            ssh("openbsd", "umount /mnt/log")
            ssh("linux", "btrfs check --readonly --check-data-csum /dev/sdb")
            assert super_info(image)["log_root"] == 0
        else:
            for option in ("", "-o ro "):
                ssh("openbsd",
                    f"if mount_btrfs {option}/dev/sd1c /mnt/log; then "
                    "umount /mnt/log; exit 1; fi")
                with image.open("rb", buffering=0) as disk:
                    disk.seek(65536)
                    assert disk.read(4096) == superblock
        print(f"PASS log fault {kind}", flush=True)


def interrupt_recovery(args):
    from run_all import Runner, Serial

    image = Path("/home/mike/obj/scratch1.img")
    for guest in ("openbsd", "linux"):
        assert " type btrfs " not in ssh(guest, "mount")
    command(["dd", f"if={args.interrupt_image}", f"of={image}", "bs=1M",
             "count=1024", "conv=notrunc,fsync", "status=none"])
    before = super_info(image)
    assert before["log_root"]
    settings = argparse.Namespace(
        image=str(image), mountpoint="/mnt/log", device="/dev/sd1c",
        vm_source="/mnt/src", vm="root@10.77.0.2",
        monitor="/tmp/monitor.sock", timeout=60, boot_timeout=120)
    runner = Runner(settings, Path(args.results))
    runner.log = sys.stdout
    runner.serial = Serial("/tmp/serial.sock", sys.stdout)
    try:
        # Recovery has written COW metadata and passed its first barrier,
        # but the old superblock must still point to an intact log forest.
        runner.crash_break(["mount_btrfs", "/dev/sd1c", "/mnt/log"],
                           ["btrfs_write_super_mirrors"])
        assert super_info(image) == before
        runner.boot()
    finally:
        runner.serial.close()
        if runner.monitor is not None:
            runner.monitor.close()
    mount("openbsd")
    worker("openbsd", "verify", args.cases[0])
    ssh("openbsd", "umount /mnt/log")
    ssh("linux", "btrfs check --readonly --check-data-csum /dev/sdb")
    assert super_info(image)["log_root"] == 0
    print("PASS interrupted recovery", flush=True)


def run(args):
    if args.fault_image:
        return faults(args)
    if args.interrupt_image:
        return interrupt_recovery(args)
    image = Path("/home/mike/obj/scratch1.img")
    results = Path(args.results)
    results.mkdir(parents=True, exist_ok=True)
    for guest in ("openbsd", "linux"):
        mounted = ssh(guest, "mount")
        assert " type btrfs " not in mounted, f"{guest} has a btrfs mount"
        command(["scp", __file__,
                 f"root@10.77.0.{2 if guest == 'openbsd' else 3}:/tmp/log_tree.py"])
    for case in args.cases:
        for writer in args.writers:
            if case == "namespace" and writer != "linux":
                continue
            name = f"{writer}-{case}-{args.nodesize}"
            ssh("linux", f"mkfs.btrfs -f -b {MIB * 1024} -n {args.nodesize} "
                f"--csum {args.csum} /dev/sdb")
            mount("linux")
            worker("linux", "seed", case)
            ssh("linux", "umount /mnt/log")
            mount(writer)
            worker(writer, "mutate", case)
            monitor(writer, "stop")
            info = super_info(image)
            (results / f"{name}.json").write_text(json.dumps(info, indent=2))
            snapshot = results / f"{name}.img"
            command(["cp", "--sparse=always", "--reflink=auto", image, snapshot])
            boot(writer)
            assert bool(info["log_root"]) != args.expect_commit, (
                f"{name}: unexpected publication {info}")
            for reader in args.readers:
                # Preserve the open QEMU backing file and its size throughout.
                command(["dd", f"if={snapshot}", f"of={image}", "bs=1M",
                         "count=1024", "conv=notrunc,fsync", "status=none"])
                mount(reader, args.readonly)
                worker(reader, "verify", case)
                ssh(reader, "umount /mnt/log")
                ssh("linux", "btrfs check --readonly --check-data-csum /dev/sdb")
                assert super_info(image)["log_root"] == 0
                mount(reader)
                worker(reader, "verify", case)
                ssh(reader, "umount /mnt/log")
            print(f"PASS {name}", flush=True)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("seed", "mutate", "verify"):
        phase, root, case = sys.argv[1:]
        if phase == "seed":
            seed(root, case)
        else:
            globals()[phase](root, case)
    else:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--cases", nargs="+", default=["data", "truncate"])
        parser.add_argument("--writers", nargs="+", default=["openbsd", "linux"])
        parser.add_argument("--readers", nargs="+", default=["linux", "openbsd"])
        parser.add_argument("--nodesize", type=int, default=4096)
        parser.add_argument("--csum", default="crc32c")
        parser.add_argument("--expect-commit", action="store_true")
        parser.add_argument("--readonly", action="store_true")
        parser.add_argument("--fault-image")
        parser.add_argument("--interrupt-image")
        parser.add_argument("--results", default="/home/mike/obj/btrfs-log-tests")
        run(parser.parse_args())
