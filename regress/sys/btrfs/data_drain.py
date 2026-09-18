#!/usr/bin/env python3
"""Writeback without publication, then overwrite/truncate/retire merged data."""
import os
from pathlib import Path
import sys

from unlink import sync

MIB = 1024 * 1024
SIZE = 16 * MIB
CUT = MIB + 17


def write(path, value, size):
    with path.open("r+b" if path.exists() else "xb", buffering=0) as stream:
        block = value * MIB
        for _ in range(size // MIB):
            assert stream.write(block) == len(block)


def pressure(base):
    # Force retained metadata promises to drain, including the last payloads.
    # Revisiting already dirty inodes must work after their refs materialize.
    for i in range(2048):
        (base / f"entry-{i}").touch()


def create(base):
    base.mkdir()
    write(base / "main", b"A", SIZE)
    # Make enough reusable data space so the crash phase needs no chunk growth.
    write(base / "spare", b"S", 80 * MIB)
    (base / "spare").unlink()
    sync(base)
    verify_original(base)


def verify_original(base):
    assert sorted(p.name for p in base.iterdir()) == ["main"]
    assert (base / "main").read_bytes() == b"A" * SIZE
    print("pre-drain committed data verified", flush=True)


def mutate(base):
    write(base / "main", b"B", SIZE)
    write(base / "temporary", b"T", 32 * MIB)
    pressure(base)
    # These reads must work after the payload buffers have been retired.
    assert (base / "main").read_bytes() == b"B" * SIZE
    assert (base / "temporary").read_bytes() == b"T" * (32 * MIB)
    # Retire complete coalesced allocations before a batched truncate can
    # publish a recovery marker and commit the generation containing them.
    (base / "temporary").unlink()
    pressure(base)
    fd = os.open(base / "main", os.O_RDWR)
    try:
        assert os.pwrite(fd, b"C" * 8192, 65536 + 13) == 8192
        os.ftruncate(fd, CUT)
        os.ftruncate(fd, SIZE)
    finally:
        os.close(fd)
    pressure(base)
    verify(base)


def verify(base):
    expected = bytearray(b"B" * CUT + bytes(SIZE - CUT))
    expected[65536 + 13:65536 + 13 + 8192] = b"C" * 8192
    assert (base / "main").read_bytes() == expected
    assert not (base / "temporary").exists()
    assert len(list(base.iterdir())) == 2049
    print("drained data splits, zero tail and final drops verified", flush=True)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
