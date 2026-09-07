#!/usr/bin/env python3
"""Durable extent coalescing, followed by reference splits and final drops."""
import mmap
import os
from pathlib import Path
import re
import sys

from checksums import payload, SECTOR
from mirrors import inspect
from unlink import sync

COUNT = 512
SIZE = COUNT * SECTOR


def original():
    return b"".join(payload(3, sector) for sector in range(COUNT))


def changed():
    data = bytearray(original())
    data[SECTOR + 13:SECTOR + 113] = b"P" * 100
    data[15 * SECTOR:33 * SECTOR] = b"W" * (18 * SECTOR)
    data[47 * SECTOR:48 * SECTOR] = b"Z" * SECTOR
    cut = 65 * SECTOR + 17
    return bytes(data[:cut]) + bytes(SIZE - cut)


def create(base):
    base.mkdir()
    with (base / "main").open("xb", buffering=0) as stream:
        for sector in range(COUNT):
            assert stream.write(payload(3, sector)) == SECTOR
        os.fsync(stream.fileno())
    os.link(base / "main", base / "alias")
    sync(base)
    verify_original(base)


def verify_original(base):
    assert (base / "main").read_bytes() == original()
    assert (base / "alias").stat().st_ino == (base / "main").stat().st_ino
    print("original coalesced data verified", flush=True)


def mutate(base):
    fd = os.open(base / "main", os.O_RDWR)
    mapping = mmap.mmap(fd, SIZE, access=mmap.ACCESS_READ)
    assert mapping[:] == original()
    mapping.close()
    assert os.pwrite(fd, b"P" * 100, SECTOR + 13) == 100
    assert os.pwrite(fd, b"W" * (18 * SECTOR), 15 * SECTOR) == 18 * SECTOR
    # Repeated pending COW at an interior offset of a committed extent.
    for value in (b"X", b"Y", b"Z"):
        assert os.pwrite(fd, value * SECTOR, 47 * SECTOR) == SECTOR
    os.fsync(fd)
    # OpenBSD's native filesystems also retain faulted mapped pages across
    # pwrite. Fault this mapping after writes, then test truncate retention.
    mapping = mmap.mmap(fd, SIZE, access=mmap.ACCESS_READ)
    assert mapping[:65 * SECTOR + 17] == changed()[:65 * SECTOR + 17]
    os.ftruncate(fd, 65 * SECTOR + 17)
    assert mapping[:65 * SECTOR + 17] == changed()[:65 * SECTOR + 17]
    mapping.close()
    os.ftruncate(fd, SIZE)
    os.fsync(fd)
    os.close(fd)
    verify(base)

    # Cancel pending sectors, then remove a coalesced allocation on last close.
    fd = os.open(base / "pending", os.O_CREAT | os.O_RDWR, 0o600)
    for sector in range(64):
        assert os.write(fd, payload(9, sector)) == SECTOR
    os.ftruncate(fd, 7 * SECTOR)
    os.fsync(fd)
    os.unlink(base / "pending")
    assert os.pread(fd, 7 * SECTOR, 0) == b"".join(
        payload(9, sector) for sector in range(7))
    os.close(fd)
    sync(base)
    print("coalesced extent splits and final drops passed", flush=True)


def verify(base):
    assert (base / "main").read_bytes() == changed()
    assert (base / "alias").read_bytes() == changed()
    assert (base / "main").stat().st_nlink == 2
    assert not (base / "pending").exists()
    print("split coalesced data verified", flush=True)


def disk(image):
    extents = []
    for item in inspect(str(image), "dump-tree", "-t", "fs").split("\titem "):
        key = re.search(r"key \((\d+) EXTENT_DATA (\d+)\)", item)
        if key is None:
            continue
        extent = re.search(r"extent data disk byte (\d+) nr (\d+)", item)
        if extent is None:
            continue
        length = int(extent[2])
        assert SECTOR <= length <= 65536 and length % SECTOR == 0
        extents.append(length)
    assert sum(extents) == SIZE, extents
    assert len(extents) < COUNT // 2, ("sector extent overhead", len(extents))
    print(f"{COUNT} sectors stored in {len(extents)} regular extents; "
          f"largest {max(extents)} bytes", flush=True)


def crash_write(base):
    fd = os.open(base / "main", os.O_RDWR)
    assert os.pwrite(fd, b"C" * (4 * SECTOR), 0) == 4 * SECTOR
    os.fsync(fd)
    os.close(fd)
    verify_crash(base)


def verify_crash(base):
    expected = b"C" * (4 * SECTOR) + original()[4 * SECTOR:]
    assert (base / "main").read_bytes() == expected
    assert (base / "alias").read_bytes() == expected
    print("published coalesced replacement verified", flush=True)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
