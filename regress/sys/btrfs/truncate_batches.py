#!/usr/bin/env python3
"""Bounded linked-file truncation, protected reservations, and recovery."""
import errno
import mmap
import os
from pathlib import Path
import sys

import enospc
from checksums import payload, SECTOR


COUNT = 512
TARGET = 3 * SECTOR + 17


def fill(fd):
    for sector in range(COUNT):
        assert os.pwrite(fd, payload(7, sector), sector * SECTOR) == SECTOR
        if sector % 128 == 127:
            os.fsync(fd)
    os.fsync(fd)


def prefix():
    return b"".join(payload(7, sector) for sector in range(4))[:TARGET]


def create(base):
    base.mkdir()
    name = base / "linked"
    fd = os.open(name, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        fill(fd)
        os.link(name, base / "alias")
        before = os.fstat(fd)
        mapping = mmap.mmap(fd, COUNT * SECTOR, access=mmap.ACCESS_READ)
        assert mapping[:TARGET] == prefix()
        os.ftruncate(fd, TARGET)
        assert os.fstat(fd).st_ino == before.st_ino
        assert os.fstat(fd).st_nlink == 2
        assert os.fstat(fd).st_blocks == 4 * SECTOR // 512
        assert mapping[:TARGET] == prefix()
        mapping.close()
        os.ftruncate(fd, COUNT * SECTOR)
        assert os.pread(fd, COUNT * SECTOR, 0) == \
            prefix() + bytes(COUNT * SECTOR - TARGET)
        os.fsync(fd)
    finally:
        os.close(fd)
    # An active zero-link inode must keep its prefix and cleanup marker.
    fd = os.open(base / "unlinked", os.O_CREAT | os.O_RDWR, 0o600)
    fill(fd)
    os.unlink(base / "unlinked")
    os.ftruncate(fd, TARGET)
    assert os.fstat(fd).st_nlink == 0
    assert os.pread(fd, COUNT * SECTOR, 0) == prefix()
    assert os.pwrite(fd, b"open", 0) == 4
    os.fsync(fd)
    os.close(fd)
    verify(base)


def verify(base):
    assert (base / "linked").read_bytes() == \
        prefix() + bytes(COUNT * SECTOR - TARGET)
    assert (base / "linked").stat().st_ino == (base / "alias").stat().st_ino
    assert not (base / "unlinked").exists()
    print("bounded linked and open-unlinked truncation verified", flush=True)


def capacity(base):
    base.mkdir()
    fd = os.open(base / "linked", os.O_CREAT | os.O_RDWR, 0o600)
    fill(fd)
    os.link(base / "linked", base / "alias")
    output = os.open(base / "filler", os.O_CREAT | os.O_RDWR, 0o600)
    enospc.main(str(base / "full"))
    while True:
        try:
            assert os.write(output, b"F" * SECTOR) == SECTOR
        except OSError as error:
            assert error.errno == errno.ENOSPC
            break
    os.fsync(output)
    before = os.statvfs(base).f_bavail
    assert before > 0, "data exhausted before the metadata reservation margin"
    os.ftruncate(fd, 0)
    assert os.fstat(fd).st_blocks == 0
    assert os.fstat(fd).st_nlink == 2
    assert (base / "alias").read_bytes() == b""
    assert os.statvfs(base).f_bavail >= before + COUNT
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    assert os.write(fd, b"reused") == 6
    os.fsync(fd)
    os.close(fd)
    os.close(output)
    print("linked truncation at capacity passed", flush=True)


def capacity_verify(base):
    assert (base / "linked").read_bytes() == b"reused"
    assert (base / "alias").read_bytes() == b"reused"
    assert (base / "linked").stat().st_nlink == 2
    print("truncation and reuse at capacity verified", flush=True)


def prepare(base):
    base.mkdir()
    fd = os.open(base / "linked", os.O_CREAT | os.O_RDWR, 0o600)
    fill(fd)
    os.close(fd)
    os.link(base / "linked", base / "alias")
    os.sync()
    print("truncate recovery fixture ready", flush=True)


def truncate(base):
    # Stop at btrfs_cleanup_inode in DDB, then reset the VM to retain
    # a durable target size and linked orphan marker before cleanup.
    os.truncate(base / "linked", TARGET)


def recovered(base):
    assert (base / "linked").read_bytes() == prefix()
    assert (base / "linked").stat().st_nlink == 2
    assert (base / "alias").read_bytes() == prefix()
    assert (base / "linked").stat().st_blocks == 4 * SECTOR // 512
    os.truncate(base / "alias", COUNT * SECTOR)
    verify(base)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]))
