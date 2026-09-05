#!/usr/bin/env python3
"""Special devices on a disposable filesystem; never writes disk devices."""
import errno
import fcntl
import os
import re
import select
import stat
import subprocess
import sys

from namespace import COLLISIONS, child_checks, expect_error, wait


# These are OpenBSD/amd64 assignments, including devices already used by FFS.
DEVICES = {
    "null": (stat.S_IFCHR, 2, 2),
    "zero": (stat.S_IFCHR, 2, 12),
    "bpf": (stat.S_IFCHR, 23, 0),
    "rootdisk": (stat.S_IFBLK, 4, 0),
    "selfdisk": (stat.S_IFBLK, 4, 66),
    "large-minor": (stat.S_IFCHR, 2, 0xabcde),
    "unknown": (stat.S_IFCHR, 255, 0xfffff),
    "unknown-block": (stat.S_IFBLK, 255, 0xfffff),
}


def seed():
    # Run as root on the host. Linux's makedev differs from the disk encoding;
    # mkfs.btrfs must translate this fixture independently of the driver.
    for name, (kind, major, minor) in DEVICES.items():
        os.mknod(name, kind | 0o600, os.makedev(major, minor))
    os.mknod("overflow-major", stat.S_IFCHR | 0o600, os.makedev(256, 2))


def fix_seed(image):
    # mkfs.btrfs 7.0 imports special-file types but initializes rdev to zero.
    # Edit the disposable seed with btrfs-progs, never with the kernel writer.
    tree = subprocess.check_output(
        ["btrfs", "inspect-internal", "dump-tree", "-t", "fs", image],
        text=True)
    devices = dict(DEVICES)
    devices["overflow-major"] = (stat.S_IFCHR, 256, 2)
    for ino, name in re.findall(
            r"key \((\d+) INODE_REF 256\).*?\bname: ([^\n]+)", tree, re.S):
        if name == "..":
            continue
        _, major, minor = devices[name]
        disk = (minor & 255) | (major << 8) | ((minor & ~255) << 12)
        for offset, byte in enumerate(disk.to_bytes(8, "little"), 56):
            subprocess.run(
                ["btrfs-corrupt-block", "-r", "5", "-I", f"{ino},1,0",
                 "--offset", str(offset), "-b", "1", "--value", str(byte),
                 image], check=True, stdout=subprocess.DEVNULL)


def verify_seed():
    for name, (kind, major, minor) in DEVICES.items():
        info = os.stat(name)
        assert stat.S_IFMT(info.st_mode) == kind, name
        assert info.st_rdev == os.makedev(major, minor), (name, info.st_rdev)
        assert (info.st_size, info.st_blocks) == (0, 0), name
    if os.path.lexists("overflow-major") or "overflow-major" in os.listdir("."):
        expect_error(errno.EOVERFLOW, os.stat, "overflow-major")


def io():
    fd = os.open("null", os.O_RDWR)
    try:
        assert os.write(fd, b"discarded") == 9
        assert os.read(fd, 64) == b""
        assert stat.S_ISCHR(os.fstat(fd).st_mode)
        os.fsync(fd)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)
    fd = os.open("zero", os.O_RDWR)
    try:
        assert os.read(fd, 8192) == bytes(8192)
        assert os.write(fd, b"discarded") == 9
        queue = select.kqueue()
        try:
            queue.control([select.kevent(fd, filter=select.KQ_FILTER_READ,
                                         flags=select.KQ_EV_ADD)], 0)
            assert queue.control([], 1, 1)
        finally:
            queue.close()
    finally:
        os.close(fd)
    for name in ("unknown", "unknown-block", "large-minor"):
        expect_error(errno.ENXIO, os.open, name, os.O_RDONLY)
    # Alias tracking must see both the root filesystem and this mount.
    for name in ("rootdisk", "selfdisk"):
        expect_error(errno.EBUSY, os.open, name, os.O_RDONLY)
    # Two concurrent clones exercise bitmap sharing with /dev/bpf and reclaim.
    descriptors = [os.open(name, os.O_RDWR)
                   for name in ("bpf", "/dev/bpf", "bpf")]
    for fd in descriptors:
        assert stat.S_ISCHR(os.fstat(fd).st_mode)
        os.close(fd)


def verify():
    verify_seed()
    assert os.stat("null").st_nlink == 2
    assert os.stat("null-alias").st_ino == os.stat("null").st_ino
    assert stat.S_IMODE(os.stat("null").st_mode) == 0o640
    assert os.stat("null").st_flags == stat.UF_NODUMP
    for name in COLLISIONS:
        assert stat.S_ISCHR(os.stat(name).st_mode)
        assert os.stat(name).st_rdev == os.makedev(2, 2)
    assert os.stat(".").st_size == sum(len(n) * 2 for n in os.listdir("."))
    io()


def create():
    os.umask(0)
    for name, (kind, major, minor) in DEVICES.items():
        os.mknod(name, kind | 0o600, os.makedev(major, minor))
    os.link("null", "null-alias")
    os.chmod("null-alias", 0o640)
    os.chflags("null-alias", stat.UF_NODUMP)
    for name in COLLISIONS:
        os.mknod(name, stat.S_IFCHR | 0o600, os.makedev(2, 2))
        fd = os.open(".", os.O_RDONLY)
        os.fsync(fd)
        os.close(fd)
    before = os.stat(".")
    expect_error(errno.EEXIST, os.mknod, "null", stat.S_IFCHR | 0o600,
                 os.makedev(2, 12))
    expect_error(errno.EOVERFLOW, os.mknod, "overflow", stat.S_IFCHR | 0o600,
                 os.makedev(2, 0x100000))
    expect_error(errno.ENOENT, os.stat, "overflow")
    after = os.stat(".")
    assert (before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    os.mkdir("public", 0o777)

    def denied():
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
        expect_error(errno.EPERM, os.mknod, "public/device",
                     stat.S_IFCHR | 0o600, os.makedev(2, 2))
        expect_error(errno.EACCES, os.open, "zero", os.O_RDONLY)

    wait(child_checks(denied))
    def aliases(worker):
        for number in range(24):
            name = f"alias-{worker}-{number}"
            os.mknod(name, stat.S_IFCHR | 0o600, os.makedev(2, 2))
            fd = os.open(name, os.O_RDWR)
            assert os.write(fd, b"alias") == 5
            os.close(fd)

    children = [child_checks(lambda w=w: aliases(w)) for w in range(4)]
    for pid in children:
        wait(pid)
    verify()


def reclaim(directory):
    descriptors = [os.open(os.path.join(directory, "devices", name), os.O_RDWR)
                   for name in ("null", "bpf")]
    subprocess.run(["umount", "-f", directory], check=True)
    # Forced unmount preserves open device vnodes through native specfs.
    assert os.write(descriptors[0], b"after unmount") == 13
    for fd in descriptors:
        os.close(fd)


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase == "fix-seed":
        fix_seed(os.path.abspath(directory))
        print("devices fix-seed passed", flush=True)
        sys.exit(0)
    if phase == "reclaim":
        reclaim(os.path.abspath(directory))
        print("devices reclaim passed", flush=True)
        sys.exit(0)
    if phase in ("seed", "create"):
        os.mkdir(directory)
    os.chdir(directory)
    if phase == "seed":
        seed()
    elif phase == "create":
        create()
    elif phase == "seed-verify":
        verify_seed()
        io()
    elif phase == "nodev":
        verify_seed()
        for name in ("null", "zero", "bpf", "rootdisk", "selfdisk"):
            expect_error(errno.ENXIO, os.open, name, os.O_RDONLY)
    else:
        verify()
        if phase == "readonly":
            expect_error(errno.EROFS, os.mknod, "readonly-device",
                         stat.S_IFCHR | 0o600, os.makedev(2, 2))
            expect_error(errno.EROFS, os.chmod, "null", 0o600)
    print(f"devices {phase} passed", flush=True)
