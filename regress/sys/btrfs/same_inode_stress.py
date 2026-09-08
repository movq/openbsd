#!/usr/bin/env python3
"""Concurrent writes through many descriptors and names of the same inode."""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import random
import signal
import struct
import sys


SECTOR = 4096
SECTORS = 512
WORKERS = 8
OPERATIONS = 3000
APPENDS = 1000


def initial(sector):
    digest = hashlib.sha256(struct.pack("<Q", sector)).digest()
    return digest * (SECTOR // len(digest))


def change(worker, operation, length):
    digest = hashlib.sha256(struct.pack("<II", worker, operation)).digest()
    return (digest * ((length + len(digest) - 1) // len(digest)))[:length]


def operation_stream(worker, count):
    rng = random.Random(0x1A0DE000 + worker)
    owned = list(range(worker, SECTORS, WORKERS))
    for operation in range(count):
        sector = rng.choice(owned)
        offset = rng.randrange(SECTOR)
        length = rng.randrange(1, SECTOR - offset + 1)
        yield operation, sector, offset, length


def expected_sector(worker, sector, count):
    data = bytearray(initial(sector))
    for operation, target, offset, length in operation_stream(worker, count):
        if target == sector:
            data[offset:offset + length] = change(worker, operation, length)
    return bytes(data)


def overwrite_worker(path, worker, count):
    fd = os.open(path, os.O_RDWR)
    try:
        for operation, sector, offset, length in operation_stream(worker, count):
            data = change(worker, operation, length)
            target = sector * SECTOR + offset
            assert os.pwrite(fd, data, target) == length
            if operation % 47 == 46:
                check_sector(fd, worker, sector, count=operation + 1)
            if operation % 113 == 112:
                os.fsync(fd)
        os.fsync(fd)
    finally:
        os.close(fd)


def check_sector(fd, worker, sector, count):
    actual = os.pread(fd, SECTOR, sector * SECTOR)
    expected = expected_sector(worker, sector, count)
    assert actual == expected, (
        worker, sector, count, hashlib.sha256(actual).hexdigest(),
        hashlib.sha256(expected).hexdigest())


def append_record(worker, operation):
    header = struct.pack("<II", worker, operation)
    digest = hashlib.sha256(header).digest()
    return header + digest + bytes(SECTOR - len(header) - len(digest))


def append_worker(path, worker, count):
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    try:
        for operation in range(count):
            record = append_record(worker, operation)
            assert os.write(fd, record) == SECTOR
            if operation % 127 == 126:
                os.fsync(fd)
        os.fsync(fd)
    finally:
        os.close(fd)


def verify_overwrite(base, count):
    path = base / "shared"
    assert path.stat().st_ino == (base / "alias").stat().st_ino
    assert path.stat().st_size == SECTORS * SECTOR
    fd = os.open(path, os.O_RDONLY)
    try:
        for sector in range(SECTORS):
            check_sector(fd, sector % WORKERS, sector, count)
    finally:
        os.close(fd)


def verify_append(base, count):
    seen = set()
    with (base / "append").open("rb", buffering=0) as stream:
        for position in range(WORKERS * count):
            record = stream.read(SECTOR)
            assert len(record) == SECTOR, (position, len(record))
            worker, operation = struct.unpack_from("<II", record)
            assert worker < WORKERS and operation < count, (
                position, worker, operation)
            assert record == append_record(worker, operation), (
                position, worker, operation)
            assert (worker, operation) not in seen, (
                position, worker, operation)
            seen.add((worker, operation))
        assert stream.read() == b""
    assert len(seen) == WORKERS * count


def create(base, count=OPERATIONS, appends=APPENDS):
    base.mkdir()
    with (base / "shared").open("xb", buffering=0) as output:
        for sector in range(SECTORS):
            assert output.write(initial(sector)) == SECTOR
        os.fsync(output.fileno())
    os.link(base / "shared", base / "alias")
    (base / "append").touch()
    paths = [base / ("shared" if worker % 2 else "alias")
             for worker in range(WORKERS)]
    with concurrent.futures.ProcessPoolExecutor(
            max_workers=WORKERS * 2) as executor:
        jobs = [executor.submit(overwrite_worker, paths[worker], worker, count)
                for worker in range(WORKERS)]
        jobs += [executor.submit(append_worker, base / "append", worker, appends)
                 for worker in range(WORKERS)]
        for job in jobs:
            job.result()
    os.sync()
    verify_overwrite(base, count)
    verify_append(base, appends)
    (base / "parameters").write_text(f"{count} {appends}\n")
    os.sync()
    print(f"same-inode stress passed: {WORKERS * count} overwrites, "
          f"{WORKERS * appends} appends", flush=True)


def verify(base):
    count, appends = map(int, (base / "parameters").read_text().split())
    verify_overwrite(base, count)
    verify_append(base, appends)
    print("persisted same-inode data verified", flush=True)


if __name__ == "__main__":
    signal.alarm(900)
    phase = sys.argv[1]
    directory = Path(sys.argv[2]).absolute()
    if phase == "create":
        count = int(sys.argv[3]) if len(sys.argv) > 3 else OPERATIONS
        appends = int(sys.argv[4]) if len(sys.argv) > 4 else APPENDS
        create(directory, count, appends)
    else:
        verify(directory)
