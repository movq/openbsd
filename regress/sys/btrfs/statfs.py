#!/usr/bin/env python3
"""Live allocator reporting and independent committed-space comparisons."""
import errno
import json
import os
from pathlib import Path
import re
import sys

from mirrors import inspect, STRIPE, SUPERS
from namespace import expect_error


def disk(image):
    """Compute expected logical space from independently decoded disk trees."""
    sector = int(re.search(r"^sectorsize\s+(\d+)",
                          inspect(image, "dump-super"), re.M)[1])
    used = {}
    for item in inspect(image, "dump-tree", "-t", "extent").split("\titem "):
        key = re.match(r"\d+ key \((\d+) BLOCK_GROUP_ITEM (\d+)\)", item)
        if key:
            used[int(key[1])] = int(re.search(
                r"block group used (\d+)", item)[1])
    total = free = available = 0
    for item in inspect(image, "dump-tree", "-t", "chunk").split("\titem "):
        key = re.match(r"\d+ key \(\S+ CHUNK_ITEM (\d+)\)", item)
        if key is None:
            continue
        start = int(key[1])
        length = int(re.search(r"\blength (\d+)", item)[1])
        flags = re.search(r"\btype (\S+)", item)[1].split("|")
        assert flags[-1] in ("single", "DUP")
        # Union sector exclusions from all physical copies.
        excluded = set()
        if start < SUPERS[0]:
            excluded.update(range(0, min(length, SUPERS[0] - start), sector))
        for physical in re.findall(r"stripe \d+ devid \d+ offset (\d+)", item):
            physical = int(physical)
            for address in SUPERS:
                if physical <= address < physical + length:
                    begin = address - physical
                    excluded.update(range(begin, min(begin + STRIPE, length),
                                          sector))
        unused = length - used.pop(start) - len(excluded) * sector
        assert unused >= 0
        total += length
        free += unused
        if "DATA" in flags:
            available += unused
    assert not used and total > 0
    print(json.dumps([total // sector, free // sector, available // sector]))


def snapshot(base):
    stats = os.statvfs(base)
    result = [stats.f_blocks, stats.f_bfree, stats.f_bavail]
    assert 0 <= result[2] <= result[1] <= result[0], result
    return result


def check(base, expected):
    actual = snapshot(base)
    assert actual == json.loads(Path(expected).read_text()), actual
    fd = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
    try:
        stats = os.fstatvfs(fd)
        assert [stats.f_blocks, stats.f_bfree, stats.f_bavail] == actual
    finally:
        os.close(fd)
    print(f"on-disk and mounted space agree: {actual}", flush=True)


def exercise(base):
    base.mkdir()
    sector = os.statvfs(base).f_frsize
    fd = os.open(base / "small", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    os.fsync(fd)
    before = snapshot(base)
    try:
        os.write(fd, b"S" * sector)
        pending = snapshot(base)
        assert pending[0] == before[0]
        assert pending[1] < before[1]  # Metadata and data already allocated.
        assert pending[2] == before[2] - 1
        os.fsync(fd)
        committed = snapshot(base)
        assert committed[2] == pending[2]
        # A COW overwrite consumes a second sector until its old extent drops.
        os.pwrite(fd, b"T" * sector, 0)
        assert snapshot(base)[2] <= committed[2]
        os.fsync(fd)
        assert snapshot(base)[2] == committed[2]
        os.ftruncate(fd, 0)
        os.fsync(fd)
        assert snapshot(base)[2] == before[2]
        # Cancelling unpublished sectors returns allocations and reservations.
        os.pwrite(fd, b"U" * sector, 0)
        os.ftruncate(fd, 0)
        os.fsync(fd)
        assert snapshot(base)[2] == before[2]
        os.pwrite(fd, b"R" * sector, 0)
        os.fsync(fd)
    finally:
        os.close(fd)
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    print("live allocation, COW, truncation, and cancellation passed", flush=True)


def capacity(base):
    sector = os.statvfs(base).f_frsize
    before = snapshot(base)
    assert 0 < before[2] * sector <= 32 * 1024 * 1024, (
        "use a fresh fixture with a small data group", before)
    fd = os.open(base / "full", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        for i in range(before[2]):
            assert os.write(fd, b"F" * sector) == sector
            if i % 128 == 127:
                os.fsync(fd)
                assert snapshot(base)[2] == before[2] - i - 1
        os.fsync(fd)
        assert snapshot(base)[2] == 0
        expect_error(errno.ENOSPC, os.write, fd, b"!")
        assert os.fstat(fd).st_size == before[2] * sector
        os.fsync(fd)
        assert snapshot(base)[2] == 0
        # Reclaim one committed extent and spend exactly that space again.
        small = os.open(base / "small", os.O_RDWR)
        os.ftruncate(small, 0)
        os.fsync(small)
        assert snapshot(base)[2] == 1
        assert os.write(small, b"R" * sector) == sector
        os.fsync(small)
        os.close(small)
        assert snapshot(base)[2] == 0
    finally:
        os.close(fd)
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    print("reported data capacity, ENOSPC, and reuse passed", flush=True)


def verify(base):
    sector = os.statvfs(base).f_frsize
    assert (base / "small").read_bytes() == b"R" * sector
    full = base / "full"
    assert full.read_bytes() == b"F" * full.stat().st_size
    assert snapshot(base)[2] == 0
    print("capacity data verified", flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "check":
        check(Path(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] in (
            "disk", "exercise", "capacity", "verify"):
        globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
    else:
        sys.exit(f"usage: {sys.argv[0]} disk|exercise|capacity|verify path\n"
                 f"       {sys.argv[0]} check mountpoint expected.json")
