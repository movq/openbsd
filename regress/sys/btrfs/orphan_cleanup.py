#!/usr/bin/env python3
"""Final-link lifetime, bounded cleanup, and crash-recovery fixtures."""
import errno
import mmap
import os
from pathlib import Path
import stat
import sys
import time

import enospc
from namespace import expect_error


def fill(fd, count):
    for i in range(count):
        os.pwrite(fd, bytes([i % 251]) * 4096, i * 8192)
        if i % 128 == 127:
            os.fsync(fd)
    os.fsync(fd)


def create(base):
    base.mkdir()
    available = os.statvfs(base).f_bavail
    name = base / "fragmented"
    fd = os.open(name, os.O_CREAT | os.O_RDWR, 0o600)
    fill(fd, 512)
    old = os.fstat(fd)
    os.link(name, base / "alias")
    os.unlink(name)
    os.unlink(base / "alias")
    assert os.fstat(fd).st_nlink == 0
    os.fchmod(fd, 0o640)
    os.pwrite(fd, b"open orphan\n", 0)
    os.fsync(fd)
    assert stat.S_IMODE(os.fstat(fd).st_mode) == 0o640
    assert os.pread(fd, 12, 0) == b"open orphan\n"
    assert os.pread(fd, 4096, 511 * 8192) == bytes([511 % 251]) * 4096
    # The removed name can identify a new inode while the old one is open.
    name.write_bytes(b"new name\n")
    assert name.stat().st_ino != old.st_ino
    os.close(fd)
    # Final cleanup can remain in the open transaction. Freed allocations
    # become available only after delayed references and pins are published.
    os.sync()
    assert os.statvfs(base).f_bavail == available - 1
    os.unlink(name)
    os.sync()
    assert os.statvfs(base).f_bavail == available

    # Inode IDs cannot be reused in the same mount after last-close cleanup.
    previous = old.st_ino
    for i in range(32):
        name.write_bytes(b"small\n")
        current = name.stat().st_ino
        assert current > previous
        previous = current
        os.unlink(name)
    fd = os.open(name, os.O_CREAT | os.O_RDWR, 0o600)
    os.write(fd, b"M" * 4096)
    mapping = mmap.mmap(fd, 4096, access=mmap.ACCESS_READ)
    os.unlink(name)
    os.close(fd)
    assert mapping[:] == b"M" * 4096
    mapping.close()
    os.symlink("target", name)
    os.unlink(name)
    os.mkfifo(name)
    fd = os.open(name, os.O_RDWR | os.O_NONBLOCK)
    os.unlink(name)
    os.write(fd, b"pipe")
    assert os.read(fd, 4) == b"pipe"
    os.close(fd)
    os.mknod(name, stat.S_IFCHR | 0o600, os.makedev(2, 2))
    os.unlink(name)

    # Cleanup of committed mappings alongside unrelated pending writes must
    # preserve those writes. Pending mappings in the removed inode may start
    # beyond sector zero; retire them before raw cleanup/coalescing.
    name.write_bytes(b"C" * 12288)
    os.sync()
    other = base / "pending"
    other.write_bytes(b"P" * 16384)
    os.unlink(name)
    fd = os.open(name, os.O_CREAT | os.O_RDWR, 0o600)
    os.pwrite(fd, b"D" * 12288, 65536)
    os.unlink(name)
    assert os.pread(fd, 12288, 65536) == b"D" * 12288
    os.close(fd)
    os.sync()
    assert other.read_bytes() == b"P" * 16384
    os.unlink(other)
    name.write_bytes(b"survivor\n")
    os.sync()
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    verify(base)


def verify(base):
    assert {p.name for p in base.iterdir()} == {"fragmented"}
    assert (base / "fragmented").read_bytes() == b"survivor\n"
    assert (base / "fragmented").stat().st_nlink == 1
    print("last-link lifetime and bounded cleanup passed", flush=True)


def hold(base):
    """Leave a committed open orphan for an externally triggered VM reset."""
    base.mkdir()
    (base / "survivor").write_bytes(b"survives reset\n")
    fd = os.open(base / "orphan", os.O_CREAT | os.O_RDWR, 0o600)
    fill(fd, 2048)
    os.unlink(base / "orphan")
    os.pwrite(fd, b"after unlink\n", 0)
    os.fsync(fd)
    os.sync()
    Path("/tmp/btrfs-orphan-ready").write_text(str(os.fstat(fd).st_ino))
    print("committed open orphan ready for VM reset", flush=True)
    while True:
        time.sleep(60)


def recovered(base):
    assert {p.name for p in base.iterdir()} == {"survivor"}
    assert (base / "survivor").read_bytes() == b"survives reset\n"
    # Recovery must leave the filesystem able to allocate and reclaim again.
    name = base / "after-recovery"
    name.write_bytes(b"recovered\n")
    os.unlink(name)
    os.sync()
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    print("orphan recovery and subsequent mutation passed", flush=True)


def capacity(base):
    """Use a rootdir fixture with at least 16 MiB of data capacity."""
    base.mkdir()
    fd = os.open(base / "orphan", os.O_CREAT | os.O_RDWR, 0o600)
    fill(fd, 32)
    os.unlink(base / "orphan")
    os.fsync(fd)
    output = os.open(base / "filler", os.O_CREAT | os.O_RDWR, 0o600)
    enospc.main(str(base / "full"))
    # Writes require smaller reservations than creation. Consume that margin
    # too, so the minimum cleanup batch must use its protected reservation.
    written = 0
    while True:
        try:
            os.write(output, b"F" * 4096)
        except OSError as error:
            assert error.errno == errno.ENOSPC
            os.fsync(output)
            if os.statvfs(base).f_bavail == 0:
                raise AssertionError("data exhausted before metadata")
            break
        written += 1
        if written % 128 == 0:
            os.fsync(output)
    before = os.statvfs(base).f_bavail
    os.close(fd)
    assert os.statvfs(base).f_bavail == before + 32
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    os.close(output)
    os.sync()
    print("protected cleanup reserve passed after metadata exhaustion", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in (
            "create", "verify", "hold", "recovered", "capacity"):
        sys.exit(f"usage: {sys.argv[0]} create|verify|hold|recovered|capacity directory")
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
