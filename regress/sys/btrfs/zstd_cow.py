#!/usr/bin/env python3
"""Sector COW and partial truncation of imported Zstd regular mappings."""
import errno
import mmap
import os
from pathlib import Path
import sys

from namespace import child_checks, expect_error, wait


# Avoid mkfs.btrfs 7.0's raw one-sector tail marked as compressed.
SIZES = (2048, 6003, 8192, 65539, 270339)
LARGE = 400003
TARGETS = (0, 1, 4096, 4103, 65537, 131072, 131079, 262144)


def original(size):
    return bytes((n * 37 + 19) % 251 for n in range(size))


def writes(size):
    return ((size // 2, b"middle"), (0, b"prefix"),
            (4093, b"boundary" * 600), (size - 1, b"tail" * 1300))


def expected(size):
    data = bytearray(original(size))
    for offset, payload in writes(size):
        end = offset + len(payload)
        if end > len(data):
            data.extend(bytes(end - len(data)))
        data[offset:end] = payload
    return bytes(data)


def seed():
    for size in SIZES:
        Path(f"write-{size}").write_bytes(original(size))
        Path(f"grow-{size}").write_bytes(original(size))
    for length in TARGETS:
        Path(f"shrink-{length}").write_bytes(original(LARGE))
    Path("split").write_bytes(original(LARGE))
    Path("capacity").write_bytes(original(65539))
    os.link("write-65539", "alias")


def write_case(size):
    fd = os.open(f"write-{size}", os.O_RDWR)
    data = bytearray(original(size))
    mapping = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
    assert mapping[:] == data
    mapping.close()
    for offset, payload in writes(size):
        assert os.pwrite(fd, payload, offset) == len(payload)
        end = offset + len(payload)
        if end > len(data):
            data.extend(bytes(end - len(data)))
        data[offset:end] = payload
        assert os.pread(fd, len(data) + 1, 0) == data
        # Native FFS also retains old pages in an active read mapping after
        # pwrite. Check a newly established mapping after each mutation.
        mapping = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
        assert mapping[:] == data[:size]
        mapping.close()
        os.fsync(fd)
    # Ordered replacement followed by committed reads of mixed mappings.
    os.pwrite(fd, b"X", 0)
    os.pwrite(fd, data[:1], 0)
    os.fsync(fd)
    os.close(fd)


def shrink_case(length):
    name = f"shrink-{length}"
    fd = os.open(name, os.O_RDWR)
    mapping = mmap.mmap(fd, LARGE, access=mmap.ACCESS_READ)
    assert mapping[:] == original(LARGE)
    os.ftruncate(fd, length)
    assert os.pread(fd, LARGE + 1, 0) == original(length)
    os.ftruncate(fd, LARGE)
    data = original(length) + bytes(LARGE - length)
    assert mapping[:] == data
    mapping.close()
    assert os.pread(fd, LARGE + 1, 0) == data
    os.fsync(fd)
    os.close(fd)


def exercise():
    for size in SIZES:
        fd = os.open(f"grow-{size}", os.O_RDWR)
        os.ftruncate(fd, LARGE)
        assert os.pread(fd, LARGE + 1, 0) == (
            original(size) + bytes(LARGE - size))
        os.fsync(fd)
        os.close(fd)
    jobs = [child_checks(lambda size=size: write_case(size)) for size in SIZES]
    for pid in jobs:
        wait(pid)
    jobs = [child_checks(lambda length=length: shrink_case(length))
            for length in TARGETS]
    for pid in jobs:
        wait(pid)
    # Repeatedly split the same compressed ownership reference, including
    # mappings with nonzero disk offsets, then drop its last reference.
    fd = os.open("split", os.O_RDWR)
    data = bytearray(original(LARGE))
    for sector in (3, 10, 5, 20, 30, 0, 40, 34, 60, 32, 80, 96, 64):
        os.pwrite(fd, b"S" * 4096, sector * 4096)
        data[sector * 4096:(sector + 1) * 4096] = b"S" * 4096
        os.fsync(fd)
        assert os.pread(fd, LARGE + 1, 0) == data
    for length in (262151, 131079, 98304, 40961, 12288, 4103, 17, 0):
        os.ftruncate(fd, length)
        del data[length:]
        assert os.pread(fd, LARGE + 1, 0) == data
        os.fsync(fd)
    assert os.fstat(fd).st_blocks == 0
    os.close(fd)
    os.sync()
    verify()


def verify(restored=False):
    for size in SIZES:
        assert Path(f"write-{size}").read_bytes() == expected(size)
        assert Path(f"grow-{size}").read_bytes() == (
            original(size) + bytes(LARGE - size))
    for length in TARGETS:
        assert Path(f"shrink-{length}").read_bytes() == (
            original(length) + bytes(LARGE - length))
    assert Path("split").read_bytes() == b""
    assert Path("capacity").read_bytes() == original(65539)
    assert Path("alias").read_bytes() == expected(65539)
    if not restored:
        assert os.stat("alias").st_ino == os.stat("write-65539").st_ino
        assert os.stat("split").st_blocks == 0


def restore():
    verify(restored=True)


def capacity():
    fd = os.open("filler", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    for sector in range(32768):
        try:
            os.pwrite(fd, b"F" * 4096, sector * 4096)
        except OSError as error:
            assert error.errno == errno.ENOSPC
            break
    else:
        raise AssertionError("use a small existing data block group")
    os.fsync(fd)
    os.close(fd)
    fd = os.open("capacity", os.O_RDWR)
    for function, args in ((os.pwrite, (fd, b"fail", 4103)),
                           (os.ftruncate, (fd, 4103)),
                           (os.ftruncate, (fd, LARGE))):
        before = os.fstat(fd)
        expect_error(errno.ENOSPC, function, *args)
        assert os.fstat(fd) == before
        assert os.pread(fd, 65540, 0) == original(65539)
        assert not os.statvfs(".").f_flag & os.ST_RDONLY
    # Aligned shrinking needs no data allocation or decompression.
    os.ftruncate(fd, 4096)
    os.fsync(fd)
    assert os.pread(fd, 4097, 0) == original(4096)
    os.close(fd)


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase == "seed":
        os.makedirs(directory)
    os.chdir(directory)
    globals()[phase]()
    print("zstd cow", phase, "ok", flush=True)
