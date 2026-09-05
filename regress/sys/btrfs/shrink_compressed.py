#!/usr/bin/env python3
"""Whole compressed-extent deletion, including files with unreadable codecs."""
import errno
import os
from pathlib import Path
import sys

from namespace import expect_error, put


SIZES = (2048, 6003, 8192, 65539)
LARGE = 400003
BOUNDARY = 131072


def original(size):
    return bytes((n * 37 + 19) % 251 for n in range(size))


def seed():
    for size in SIZES:
        Path(f"zero-{size}").write_bytes(original(size))
        if size > 4096:
            Path(f"reject-{size}").write_bytes(original(size))
    Path("boundary").write_bytes(original(LARGE))
    Path("regrow").write_bytes(original(LARGE))
    os.link("zero-65539", "alias")


def exercise(zstd=False):
    for size in SIZES:
        name = f"zero-{size}"
        ino = os.stat(name).st_ino
        fd = os.open(name, os.O_WRONLY | os.O_TRUNC)
        assert os.fstat(fd).st_size == 0
        assert os.fstat(fd).st_blocks == 0
        assert os.fstat(fd).st_ino == ino
        os.fsync(fd)
        os.close(fd)
    assert Path("alias").read_bytes() == b""
    assert os.stat("alias").st_nlink == 2
    for size in SIZES:
        if size <= 4096:
            continue
        name = f"reject-{size}"
        for length in (1, 4096, size - 1):
            # Failure must leave unrelated pending work committable.
            put(f"survivor-{size}-{length}", b"pending")
            before = os.stat(name)
            if zstd:
                os.truncate(name, length)
                os.truncate(name, size)
                fd = os.open(name, os.O_WRONLY)
                os.pwrite(fd, original(size), 0)
                os.close(fd)
            else:
                expect_error(errno.EOPNOTSUPP, os.truncate, name, length)
                assert os.stat(name) == before
            assert not os.statvfs(".").f_flag & os.ST_RDONLY
            os.sync()
    # Imported files have 128 KiB compressed mappings. Retain the first
    # mapping intact while dropping all subsequent ones.
    os.truncate("boundary", BOUNDARY)
    assert os.stat("boundary").st_size == BOUNDARY
    assert os.stat("boundary").st_blocks * 512 == BOUNDARY
    os.truncate("regrow", BOUNDARY)
    os.truncate("regrow", LARGE)
    fd = os.open("regrow", os.O_WRONLY)
    os.pwrite(fd, b"tail", LARGE - 4)
    os.fsync(fd)
    os.close(fd)
    os.sync()
    verify_metadata()


def exercise_zstd():
    exercise(zstd=True)


def verify_metadata():
    for size in SIZES:
        info = os.stat(f"zero-{size}")
        assert info.st_size == 0 and info.st_blocks == 0
        if size > 4096:
            assert os.stat(f"reject-{size}").st_size == size
            for length in (1, 4096, size - 1):
                assert Path(f"survivor-{size}-{length}").read_bytes() == b"pending"
    assert os.stat("alias").st_ino == os.stat("zero-65539").st_ino
    assert os.stat("boundary").st_size == BOUNDARY
    assert os.stat("regrow").st_size == LARGE


def verify():
    # Also runs on btrfs restore output for codecs OpenBSD cannot read.
    for size in SIZES:
        assert Path(f"zero-{size}").read_bytes() == b""
        if size > 4096:
            assert Path(f"reject-{size}").read_bytes() == original(size)
    assert Path("alias").read_bytes() == b""
    assert Path("boundary").read_bytes() == original(BOUNDARY)
    assert Path("regrow").read_bytes() == (
        original(BOUNDARY) + bytes(LARGE - BOUNDARY - 4) + b"tail")


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase == "seed":
        os.makedirs(directory)
    os.chdir(directory)
    globals()[phase]()
    print("shrink compressed", phase, "ok", flush=True)
