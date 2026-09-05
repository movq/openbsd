#!/usr/bin/env python3
"""Packed checksum updates, concurrent writers, deletion, and disk layout."""
import os
from pathlib import Path
import re
import subprocess
import sys

from namespace import child_checks, wait


SECTOR = 4096


def payload(worker, sector):
    return (f"{worker}:{sector:08d}\n".encode() * SECTOR)[:SECTOR]


def writer(base, worker):
    fd = os.open(base / str(worker), os.O_CREAT | os.O_EXCL | os.O_RDWR,
                 0o600)
    try:
        for sector in range(256):
            assert os.write(fd, payload(worker, sector)) == SECTOR
        os.fsync(fd)
        # Final drops split shared checksum items, while other vnodes may
        # append or replace checksums in the same tree.
        for sector in range(0, 256, 3):
            assert os.pwrite(fd, payload(worker + 10, sector),
                             sector * SECTOR) == SECTOR
        os.fsync(fd)
        # Replace pending payloads/checksums, then cancel the tail.
        for sector in range(128, 256):
            assert os.pwrite(fd, b"X" * SECTOR, sector * SECTOR) == SECTOR
            assert os.pwrite(fd, b"Y" * SECTOR, sector * SECTOR) == SECTOR
        os.ftruncate(fd, 128 * SECTOR)
        os.fsync(fd)
    finally:
        os.close(fd)


def create(base):
    base.mkdir()
    with (base / "packed").open("wb", buffering=0) as stream:
        for sector in range(1024):
            assert stream.write(payload(9, sector)) == SECTOR
            if sector % 128 == 127:
                os.fsync(stream.fileno())
    children = [child_checks(lambda w=w: writer(base, w)) for w in range(4)]
    for pid in children:
        wait(pid)
    verify(base)


def verify(base):
    with (base / "packed").open("rb") as stream:
        for sector in range(1024):
            assert stream.read(SECTOR) == payload(9, sector)
        assert stream.read() == b""
    for worker in range(4):
        with (base / str(worker)).open("rb") as stream:
            for sector in range(128):
                tag = worker + 10 if sector % 3 == 0 else worker
                assert stream.read(SECTOR) == payload(tag, sector)
            assert stream.read() == b""
    print("packed checksum data verified", flush=True)


def disk(image):
    tree = subprocess.check_output(
        ["btrfs", "inspect-internal", "dump-tree", "-t", "csum", str(image)],
        text=True)
    sizes = [int(size) for size in re.findall(
        r"key \(\S+ EXTENT_CSUM \d+\) itemoff \d+ itemsize (\d+)", tree)]
    assert sizes and max(sizes) >= 512, sizes
    assert sum(sizes) > len(sizes) * 4 * 4, (
        "expected multiple sectors per checksum item", len(sizes), sum(sizes))
    print(f"{sum(sizes) // 4} checksums in {len(sizes)} items")


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]))
