#!/usr/bin/env python3
"""Pending range replacement, short truncation, and partial-copy ownership."""
import os
from pathlib import Path
import random
import sys

from read_range import write_fault

SECTOR = 4096
WINDOW = 65536


def operations():
    yield "write", 0, b"A" * (4 * WINDOW)
    yield "write", SECTOR + 17, b"B" * (2 * WINDOW + 31)
    # Keep only a prefix of a pending allocation, then write inside the
    # discarded suffix. Its payload still needs complete on-disk checksums.
    yield "truncate", SECTOR + 53, None
    yield "write", 3 * SECTOR + 11, b"C" * WINDOW
    yield "truncate", 2 * WINDOW, None
    yield "write", WINDOW - 9, b"D" * (WINDOW + 17)
    yield "truncate", 0, None
    yield "write", 0, b"E" * (3 * WINDOW)
    rng = random.Random(17419)
    for index in range(96):
        if index % 7 == 0:
            yield "truncate", rng.randrange(4 * WINDOW), None
        else:
            yield "write", rng.randrange(4 * WINDOW), bytes(
                [index]) * rng.randrange(1, 2 * WINDOW)
        if index % 23 == 22:
            yield "sync", 0, None


def exercise(base, create=False):
    if create:
        base.mkdir()
    model = bytearray()
    fd = os.open(base / "ranges", os.O_CREAT | os.O_EXCL | os.O_RDWR
                 if create else os.O_RDONLY, 0o600)
    try:
        for operation, offset, data in operations():
            if operation == "write":
                if create:
                    assert os.pwrite(fd, data, offset) == len(data)
                end = offset + len(data)
                if end > len(model):
                    model.extend(bytes(end - len(model)))
                model[offset:end] = data
            elif operation == "truncate":
                if create:
                    os.ftruncate(fd, offset)
                del model[offset:]
                if offset > len(model):
                    model.extend(bytes(offset - len(model)))
            elif create:
                os.fsync(fd)
            if create:
                assert os.pread(fd, len(model) + WINDOW, 0) == model
        assert os.pread(fd, len(model) + WINDOW, 0) == model
        if create:
            os.fsync(fd)
    finally:
        os.close(fd)

    for length in (17, SECTOR + 17, WINDOW - 7, WINDOW + 17):
        content = b"F" * length
        fd = os.open(base / f"fault-{length}",
                     os.O_CREAT | os.O_EXCL | os.O_RDWR if create
                     else os.O_RDONLY, 0o600)
        try:
            if create:
                write_fault(fd, 0, content, WINDOW)
                assert os.pread(fd, 3 * WINDOW, 0) == content
                # Reuse the unpublished allocation's trimmed suffix as a
                # hole, and then drop the shortened owner altogether.
                os.ftruncate(fd, 7)
                assert os.pwrite(fd, b"G" * 17, WINDOW - 23) == 17
                os.fsync(fd)
            expected = b"F" * 7 + bytes(WINDOW - 30) + b"G" * 17
            assert os.pread(fd, 3 * WINDOW, 0) == expected
        finally:
            os.close(fd)
    print("range writes, pending truncation and copy-fault prefixes verified",
          flush=True)


def create(base):
    exercise(base, True)


def verify(base):
    exercise(base)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
