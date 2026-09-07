#!/usr/bin/env python3
"""Clustered physical reads, allocation reuse, and sector-level DUP recovery."""
import errno
import json
import os
from pathlib import Path
import random
import re
import resource
import sys

from checksums import payload, SECTOR
from mirrors import inspect
from namespace import expect_error
from reclaim_chunks import chunks
from unlink import sync

COUNT = 512


def fill(path):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    for sector in range(COUNT):
        assert os.write(fd, payload(13, sector)) == SECTOR
    os.fsync(fd)
    os.close(fd)


def create(base):
    base.mkdir()
    fill(base / "data")
    sync(base)


def seed(base):
    base.mkdir()
    (base / "data").write_bytes(b"".join(
        payload(13, sector) for sector in range(COUNT)))


def check(path, count):
    fd = os.open(path, os.O_RDONLY)
    try:
        for sector in range(count):
            assert os.pread(fd, SECTOR, sector * SECTOR) == payload(13, sector)
        assert os.pread(fd, 1, count * SECTOR) == b""
    finally:
        os.close(fd)


def measure(base):
    before = resource.getrusage(resource.RUSAGE_SELF).ru_inblock
    check(base / "data", COUNT)
    reads = resource.getrusage(resource.RUSAGE_SELF).ru_inblock - before
    # bio_doread charges both the file-buffer misses and physical I/O.
    # Run after create/unmount/remount for a cold sample.
    assert reads < COUNT + COUNT // 2, ("sector-sized physical reads", reads)
    print(f"{COUNT} sectors verified using {reads} input operations "
          "(including file-buffer reads)", flush=True)


def small(base):
    # Populate the large physical windows before freeing their allocation.
    check(base / "data", COUNT)
    os.truncate(base / "data", 0)
    sync(base)
    for sector in range(64):
        fd = os.open(base / f"small-{sector}", os.O_CREAT | os.O_EXCL |
                     os.O_RDWR, 0o600)
        assert os.write(fd, payload(9, sector)) == SECTOR
        os.fsync(fd)
        os.close(fd)
    sync(base)
    print("large read windows reused for sector allocations", flush=True)


def verify_small(base):
    assert (base / "data").read_bytes() == b""
    for sector in range(64):
        assert (base / f"small-{sector}").read_bytes() == payload(9, sector)
    print("sector allocations verified", flush=True)


def again(base):
    # After remount, this read populates physical sector buffers.
    verify_small(base)
    for sector in range(64):
        (base / f"small-{sector}").unlink()
    fill(base / "again")
    # Invalidate file buffers before reading the new allocation windows.
    os.truncate(base / "again", (COUNT - 1) * SECTOR)
    sync(base)
    verify_again(base)


def verify_again(base):
    assert {p.name for p in base.iterdir()} == {"data", "again"}
    assert (base / "data").read_bytes() == b""
    check(base / "again", COUNT - 1)
    print("sector buffers reused for larger read windows", flush=True)


def split(base):
    os.link(base / "data", base / "alias")
    fd = os.open(base / "data", os.O_RDWR)
    for sector in (17, 33, 129):
        assert os.pwrite(fd, payload(14, sector), sector * SECTOR) == SECTOR
    os.fsync(fd)
    os.ftruncate(fd, (COUNT - 1) * SECTOR)
    os.ftruncate(fd, COUNT * SECTOR)
    os.fsync(fd)
    os.close(fd)
    verify_split(base)


def verify_split(base):
    expected = b"".join(payload(14 if sector in (17, 33, 129) else 13,
                                sector) for sector in range(COUNT - 1))
    expected += bytes(SECTOR)
    assert (base / "data").read_bytes() == expected
    assert (base / "alias").read_bytes() == expected
    assert (base / "data").stat().st_nlink == 2
    print("split mappings verified across allocation read windows", flush=True)


def damage(image, journal):
    # A fresh create fixture contains one regular file.
    tree = inspect(str(image), "dump-tree", "-t", "fs")
    extents = re.findall(
        r"key \(\d+ EXTENT_DATA 0\).*?"
        r"extent data disk byte (\d+) nr (\d+)", tree, re.S)
    assert len(extents) == 1
    logical, length = map(int, extents[0])
    assert length >= 4 * SECTOR
    chunk = next(c for c in chunks(image)
                 if c["logical"] <= logical < c["logical"] + c["length"])
    assert len(chunk["physical"]) == 2
    stripes = [p + logical - chunk["logical"] for p in chunk["physical"]]
    fd = os.open(image, os.O_RDWR)
    saved = []
    try:
        # Sector zero is unrecoverable. Sectors one and two require different
        # mirrors of the same window; sector three remains healthy on both.
        for mirror, sector in ((0, 0), (1, 0), (0, 1), (1, 2)):
            address = stripes[mirror] + sector * SECTOR + 43
            value = os.pread(fd, 1, address)
            assert value == payload(13, sector)[43:44]
            saved.append((address, value[0]))
        journal.write_text(json.dumps(saved))
        for address, value in saved:
            assert os.pwrite(fd, bytes([value ^ 0xff]), address) == 1
        os.fsync(fd)
    finally:
        os.close(fd)
    print("damaged different sectors on both data mirrors", flush=True)


def faults(base):
    fd = os.open(base / "data", os.O_RDONLY)
    try:
        for sector in (1, 2):
            assert os.pread(fd, SECTOR, sector * SECTOR) == payload(13, sector)
        # A range can need a different mirror for each of its sectors.
        pair = payload(13, 1) + payload(13, 2)
        assert os.pread(fd, 2 * SECTOR - 26, SECTOR + 13) == pair[13:-13]
        expect_error(errno.EIO, os.pread, fd, SECTOR, 0)
        # The failed sector retry replaced full windows at the same start.
        # A new sector read must safely restore the larger buffer size.
        assert os.pread(fd, SECTOR, 3 * SECTOR) == payload(13, 3)
        for sector in range(4, COUNT):
            assert os.pread(fd, SECTOR, sector * SECTOR) == payload(13, sector)
    finally:
        os.close(fd)
    print("per-sector mirror recovery and failed-window retry passed", flush=True)


def repair(image, journal):
    fd = os.open(image, os.O_RDWR)
    try:
        for address, value in json.loads(journal.read_text()):
            assert os.pwrite(fd, bytes([value]), address) == 1
        os.fsync(fd)
    finally:
        os.close(fd)
    print("data mirror fixture restored", flush=True)


def compressed_data():
    # Keep the first sector compressible too: mkfs.btrfs 7.0 can store an
    # incompressible import raw while still marking its extent compressed.
    sample = random.Random(91).randbytes(32768)
    return (bytes(98304) + sample) * 8


def seed_compressed(base):
    base.mkdir()
    (base / "compressed").write_bytes(compressed_data())


def disk_compressed(image):
    tree = inspect(str(image), "dump-tree", "-t", "fs")
    extents = re.findall(
        r"extent data disk byte \d+ nr (\d+)\s+"
        r"extent data offset 0 nr (\d+) ram (\d+)\s+"
        r"extent compression 3 \(zstd\)", tree)
    assert len(extents) == 8, extents
    for disk, length, ram in extents:
        assert SECTOR < int(disk) <= 131072
        assert int(length) == int(ram) == 131072
    print("multi-sector Zstd allocations verified", flush=True)


def compressed(base):
    before = resource.getrusage(resource.RUSAGE_SELF).ru_inblock
    data = (base / "compressed").read_bytes()
    reads = resource.getrusage(resource.RUSAGE_SELF).ru_inblock - before
    assert data == compressed_data()
    assert reads < len(data) // SECTOR + 64, reads
    print(f"compressed data verified using {reads} input operations "
          "(including file-buffer reads)", flush=True)


if __name__ == "__main__":
    phase, *paths = sys.argv[1:]
    globals()[phase](*(Path(p).absolute() for p in paths))
