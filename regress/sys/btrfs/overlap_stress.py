#!/usr/bin/env python3
"""Concurrent overlapping writes and truncates on one inode."""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import random
import signal
import struct
import sys


MAGIC = b"BTRFSOVERLAP"
SECTOR = 4096
SECTORS = 512
WORKERS = 8
OPERATIONS = 5000


def payload(worker, operation, sector):
    header = MAGIC + struct.pack("<III", worker, operation, sector)
    seed = hashlib.sha256(header).digest()
    body = (seed * ((SECTOR - len(header) + len(seed) - 1) //
                    len(seed)))[:SECTOR - len(header)]
    return header + body


def operations(worker, count):
    rng = random.Random(0x0A11CE00 + worker)
    for operation in range(count):
        if rng.randrange(100) < 82:
            yield operation, "write", rng.randrange(SECTORS)
        else:
            yield operation, "truncate", rng.randrange(SECTORS + 1)


def worker(path, worker_number, count):
    fd = os.open(path, os.O_RDWR)
    try:
        for operation, kind, value in operations(worker_number, count):
            if kind == "write":
                data = payload(worker_number, operation, value)
                assert os.pwrite(fd, data, value * SECTOR) == SECTOR
            else:
                os.ftruncate(fd, value * SECTOR)
            if operation % 127 == 126:
                os.fsync(fd)
        os.fsync(fd)
    finally:
        os.close(fd)


def verify_file(path, count):
    fd = os.open(path, os.O_RDONLY)
    try:
        size = os.fstat(fd).st_size
        assert size % SECTOR == 0 and size <= SECTORS * SECTOR, size
        for sector in range(size // SECTOR):
            data = os.pread(fd, SECTOR, sector * SECTOR)
            assert len(data) == SECTOR, (sector, len(data))
            if data == bytes(SECTOR):
                continue
            assert data.startswith(MAGIC), (
                sector, hashlib.sha256(data).hexdigest())
            worker_number, operation, claimed_sector = struct.unpack_from(
                "<III", data, len(MAGIC))
            assert 0 <= worker_number < WORKERS, (
                sector, worker_number, operation)
            assert 0 <= operation < count, (
                sector, worker_number, operation)
            assert claimed_sector == sector, (
                sector, worker_number, operation, claimed_sector)
            expected = payload(worker_number, operation, sector)
            assert data == expected, (
                sector, worker_number, operation,
                hashlib.sha256(data).hexdigest(),
                hashlib.sha256(expected).hexdigest())
    finally:
        os.close(fd)


def create(base, count=OPERATIONS):
    base.mkdir()
    path = base / "shared"
    with path.open("xb") as output:
        output.truncate(SECTORS * SECTOR)
        os.fsync(output.fileno())
    with concurrent.futures.ProcessPoolExecutor(
            max_workers=WORKERS) as executor:
        jobs = [executor.submit(worker, path, number, count)
                for number in range(WORKERS)]
        for job in jobs:
            job.result()
    with path.open("rb") as source:
        os.fsync(source.fileno())
    verify_file(path, count)
    (base / "parameters").write_text(f"{count}\n")
    os.sync()
    print(f"overlap stress passed: {WORKERS * count} operations", flush=True)


def verify(base):
    count = int((base / "parameters").read_text())
    verify_file(base / "shared", count)
    print("persisted overlapping writes and truncates verified", flush=True)


if __name__ == "__main__":
    signal.alarm(1800)
    phase = sys.argv[1]
    directory = Path(sys.argv[2]).absolute()
    if phase == "create":
        count = int(sys.argv[3]) if len(sys.argv) > 3 else OPERATIONS
        create(directory, count)
    else:
        verify(directory)
