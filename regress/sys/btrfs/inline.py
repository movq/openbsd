#!/usr/bin/env python3
"""Import with mkfs --rootdir, then write/check/remount/verify inline files."""
import errno
import mmap
import os
from pathlib import Path
import sys

from namespace import child_checks, expect_error, wait


CASES = {
    "overwrite": (100, [(7, b"replacement"), (2, b"again")]),
    "one": (1, [(0, b"X")]),
    "append": (2048, [(2048, b"A" * 6000)]),
    "gap": (100, [(3017, b"gap")]),
    "boundary": (2048, [(4095, b"boundary")]),
    "sparse": (2048, [(16387, b"sparse"), (3, b"prefix")]),
    "sector": (100, [(4096, b"S" * 4096)]),
    "replace": (2048, [(0, b"R" * 9000)]),
}


def original(size):
    return bytes((n * 37 + 19) % 251 for n in range(size))


def expected(size, writes):
    data = bytearray(original(size))
    for offset, payload in writes:
        end = offset + len(payload)
        if end > len(data):
            data.extend(bytes(end - len(data)))
        data[offset:end] = payload
    return bytes(data)


def seed():
    for name, (size, _) in CASES.items():
        Path(name).write_bytes(original(size))
    os.link("sparse", "alias")
    Path("capacity-inline").write_bytes(original(100))


GROWTH = ((511, 512), (2048, 4096), (2048, 16387), (2047, 8193))


def seed_zstd():
    seed()
    Path("compressed-overwrite").write_bytes(original(2048))
    Path("capacity-compressed").write_bytes(original(2048))
    for size, end in GROWTH:
        Path(f"grow-{size}-{end}").write_bytes(original(size))


def write_zstd():
    write()
    write_case("compressed-overwrite", 2048, [(7, b"replacement")])
    for size, end in GROWTH:
        name = f"grow-{size}-{end}"
        fd = os.open(name, os.O_RDWR)
        try:
            assert os.read(fd, size + 1) == original(size)
            os.ftruncate(fd, end)
            assert os.pread(fd, end + 1, 0) == original(size) + bytes(end - size)
            assert os.fstat(fd).st_blocks == 8
            os.fsync(fd)
        finally:
            os.close(fd)
    verify_zstd()


def verify_zstd(restored=False):
    verify(restored)
    assert Path("compressed-overwrite").read_bytes() == expected(
        2048, [(7, b"replacement")])
    assert Path("capacity-compressed").read_bytes() == original(2048)
    for size, end in GROWTH:
        assert Path(f"grow-{size}-{end}").read_bytes() == (
            original(size) + bytes(end - size))


def verify_seed():
    for name, (size, _) in CASES.items():
        assert Path(name).read_bytes() == original(size), name
    assert Path("alias").read_bytes() == original(2048)
    assert Path("capacity-inline").read_bytes() == original(100)


def write_case(name, size, writes):
    fd = os.open(name, os.O_RDWR)
    assert os.read(fd, size + 1) == original(size)
    mapping = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
    assert mapping[:] == original(size)
    mapping.close()
    done = []
    for offset, payload in writes:
        assert os.pwrite(fd, payload, offset) == len(payload)
        done.append((offset, payload))
        data = expected(size, done)
        assert os.pread(fd, len(data) + 1, 0) == data
        os.fsync(fd)
        assert os.pread(fd, len(data) + 1, 0) == data
    # Same-transaction replacement uses ordered data rather than disk.
    assert os.pwrite(fd, b"Q", 0) == 1
    data = expected(size, writes)
    assert os.pwrite(fd, data[:1], 0) == 1
    os.fsync(fd)
    os.close(fd)


def write():
    children = [child_checks(lambda name=name, size=size, writes=writes:
                             write_case(name, size, writes))
                for name, (size, writes) in CASES.items()]
    for pid in children:
        wait(pid)
    verify()


def verify(restored=False):
    for name, (size, writes) in CASES.items():
        data = expected(size, writes)
        assert Path(name).read_bytes() == data, name
        info = os.stat(name)
        assert info.st_size == len(data)
        assert info.st_blocks > 0
    assert Path("alias").read_bytes() == expected(*CASES["sparse"])
    if not restored:
        assert os.stat("alias").st_ino == os.stat("sparse").st_ino
        assert os.stat("sparse").st_nlink == 2
    assert Path("capacity-inline").read_bytes() == original(100)


def capacity(name="capacity-inline", size=100):
    fd = os.open("filler", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        for sector in range(32768):
            try:
                os.pwrite(fd, b"F" * 4096, sector * 4096)
            except OSError as error:
                assert error.errno == errno.ENOSPC
                break
        else:
            raise AssertionError("use an image with a small data block group")
        os.fsync(fd)
    finally:
        os.close(fd)
    before = os.stat(name)
    fd = os.open(name, os.O_RDWR)
    try:
        # Need space for the preserved inline prefix and the distant write.
        expect_error(errno.ENOSPC, os.pwrite, fd, b"fail", 16384)
        expect_error(errno.ENOSPC, os.ftruncate, fd, 16384)
    finally:
        os.close(fd)
    after = os.stat(name)
    assert (before.st_size, before.st_blocks, before.st_mtime_ns,
            before.st_ctime_ns) == (
                after.st_size, after.st_blocks, after.st_mtime_ns,
                after.st_ctime_ns)
    assert Path(name).read_bytes() == original(size)
    assert not os.statvfs(".").f_flag & os.ST_RDONLY
    os.sync()
    assert not os.statvfs(".").f_flag & os.ST_RDONLY


def reject():
    # Import the seed fixture with --compress zlib for this phase.
    before = os.stat("sparse")
    fd = os.open("sparse", os.O_RDWR)
    try:
        for offset in (0, 16384):
            expect_error(errno.EOPNOTSUPP, os.pwrite, fd, b"fail", offset)
        os.fsync(fd)
    finally:
        os.close(fd)
    after = os.stat("sparse")
    assert (before.st_size, before.st_blocks, before.st_mtime_ns,
            before.st_ctime_ns) == (
                after.st_size, after.st_blocks, after.st_mtime_ns,
                after.st_ctime_ns)
    # Zlib reads are not implemented; verify content with btrfs restore.
    expect_error(errno.EOPNOTSUPP, Path("sparse").read_bytes)
    assert not os.statvfs(".").f_flag & os.ST_RDONLY


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase in ("seed", "seed-zstd"):
        os.mkdir(directory)
    os.chdir(directory)
    {"seed": seed, "write": write, "verify": verify,
     "seed-zstd": seed_zstd, "write-zstd": write_zstd,
     "verify-zstd": verify_zstd,
     "verify-restored-zstd": lambda: verify_zstd(restored=True),
     "capacity-zstd": lambda: capacity("capacity-compressed", 2048),
     "capacity": capacity, "reject": reject,
     "verify-seed": verify_seed}[phase]()
    print(f"inline {phase} passed", flush=True)
