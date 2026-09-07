#!/usr/bin/env python3
"""Unaligned range reads across pending writes, committed mappings and holes."""
import ctypes
import errno
import mmap
import os
from pathlib import Path
import random
import sys

SECTOR = 4096
WINDOW = 65536
SIZE = 4 * WINDOW + 173


def original():
    return random.Random(71).randbytes(SIZE)


def changes():
    return [(SECTOR - 7, b"A" * 31),
            (WINDOW - 19, b"B" * (WINDOW + 43)),
            (2 * WINDOW + 3, b"C" * 100),
            (2 * WINDOW + 3, b"D" * 100),
            (SIZE + SECTOR + 11, b"E" * 213)]


def expected():
    data = bytearray(original())
    for offset, value in changes():
        if offset > len(data):
            data.extend(bytes(offset - len(data)))
        data[offset:offset + len(value)] = value
    cut = 3 * WINDOW + 13
    return bytes(data[:cut]) + bytes(SIZE - cut)


def copy_faults(base, create=False):
    class Iovec(ctypes.Structure):
        _fields_ = [("base", ctypes.c_void_p), ("length", ctypes.c_size_t)]

    data = bytearray(original()[:2 * SECTOR])
    fd = os.open(base / "copy-fault", os.O_RDWR | os.O_CREAT if create
                 else os.O_RDONLY, 0o600)
    try:
        if create:
            assert os.write(fd, data) == len(data)
            os.fsync(fd)
            libc = ctypes.CDLL(None, use_errno=True)
            libc.writev.argtypes = [ctypes.c_int, ctypes.POINTER(Iovec),
                                    ctypes.c_int]
            libc.writev.restype = ctypes.c_ssize_t
        for offset, value in ((0, b"F" * 17), (11, b"G" * 17),
                              (SECTOR + 7, b"H" * 17)):
            if create:
                source = ctypes.create_string_buffer(value)
                vectors = (Iovec * 2)(
                    Iovec(ctypes.addressof(source), len(value)),
                    Iovec(None, SECTOR - len(value)))
                os.lseek(fd, offset, os.SEEK_SET)
                ctypes.set_errno(0)
                assert libc.writev(fd, vectors, 2) == -1
                assert ctypes.get_errno() == errno.EFAULT
            data[offset:offset + len(value)] = value
            if create:
                # The first iovec was copied before the second faulted.
                # Preserve all remaining bytes of committed/pending sectors.
                check(fd, data)
        if create:
            os.fsync(fd)
        check(fd, data)
    finally:
        os.close(fd)


def check(fd, data):
    assert os.pread(fd, len(data) + WINDOW, 0) == data
    for offset in (0, 1, SECTOR - 7, SECTOR + 13, WINDOW - 19,
                   WINDOW + 3, 2 * WINDOW - 1, 3 * WINDOW + 7,
                   len(data) - 1, len(data), len(data) + 31):
        for size in (1, 17, SECTOR + 19, WINDOW - 1, WINDOW + 71):
            assert os.pread(fd, size, offset) == data[offset:offset + size], (
                offset, size)
    with mmap.mmap(fd, len(data), access=mmap.ACCESS_READ) as mapping:
        assert mapping[:] == data


def create(base):
    base.mkdir()
    fd = os.open(base / "data", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        data = bytearray(original())
        assert os.write(fd, data) == len(data)
        check(fd, data)
        os.fsync(fd)
        check(fd, data)
        for offset, value in changes():
            assert os.pwrite(fd, value, offset) == len(value)
            if offset > len(data):
                data.extend(bytes(offset - len(data)))
            data[offset:offset + len(value)] = value
            # Re-fault a new mapping after each write, as on native FFS.
            check(fd, data)
        os.ftruncate(fd, 3 * WINDOW + 13)
        os.ftruncate(fd, SIZE)
        check(fd, expected())
        os.fsync(fd)
        check(fd, expected())
    finally:
        os.close(fd)
    copy_faults(base, create=True)
    print("pending, committed and sparse range reads verified", flush=True)


def verify(base):
    fd = os.open(base / "data", os.O_RDONLY)
    try:
        check(fd, expected())
    finally:
        os.close(fd)
    copy_faults(base)
    print("range reads verified after remount", flush=True)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
