#!/usr/bin/env python3
"""Concurrent open-orphan cleanup with shared extents and live survivors."""
import fcntl
import hashlib
import multiprocessing
import os
from pathlib import Path
import random
import signal
import struct
import sys


SECTOR = 4096
SECTORS = 1024
WORKERS = 8
ROUNDS = 40
OPEN_ORPHANS = 4


def payload(owner, sector, version):
    digest = hashlib.sha256(struct.pack("<III", owner, sector, version)).digest()
    return digest * (SECTOR // len(digest))


def clone(source, destination):
    control = os.open("/dev/btrfs-control", os.O_RDWR)
    try:
        fcntl.ioctl(control, 0x8020420b, struct.pack(
            "=iiQQQ", source, destination, 0, 0, SECTORS * SECTOR))
    finally:
        os.close(control)


def write_initial(fd, owner):
    for sector in range(SECTORS):
        data = payload(owner, sector, 0)
        assert os.pwrite(fd, data, sector * SECTOR) == SECTOR


def worker(base, number, rounds):
    rng = random.Random(0x0A0F0000 + number)
    anchor = os.open(base / "anchor", os.O_RDONLY)
    survivor_path = base / f"survivor-{number}"
    survivor = os.open(survivor_path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    versions = [0] * SECTORS
    write_initial(survivor, number + 1)
    os.fsync(survivor)
    try:
        for round_number in range(rounds):
            orphans = []
            for slot in range(OPEN_ORPHANS):
                path = base / f"orphan-{number}-{slot}"
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
                clone(anchor, fd)
                for _ in range(24):
                    sector = rng.randrange(SECTORS)
                    data = payload(100 + number, sector, round_number + 1)
                    assert os.pwrite(fd, data, sector * SECTOR) == SECTOR
                os.fsync(fd)
                os.unlink(path)
                for _ in range(24):
                    sector = rng.randrange(SECTORS)
                    data = payload(200 + number, sector, round_number + 1)
                    assert os.pwrite(fd, data, sector * SECTOR) == SECTOR
                os.fsync(fd)
                assert os.fstat(fd).st_nlink == 0
                orphans.append(fd)

            # Last close starts restartable cleanup of many references to the
            # same anchor allocations while other processes still mutate data.
            for fd in orphans:
                os.close(fd)
            for _ in range(64):
                sector = rng.randrange(SECTORS)
                versions[sector] += 1
                data = payload(number + 1, sector, versions[sector])
                assert os.pwrite(survivor, data, sector * SECTOR) == SECTOR
            os.fsync(survivor)
            if round_number % 5 == 4:
                check_file(survivor_path, number + 1, versions)
        os.fsync(survivor)
        check_file(survivor_path, number + 1, versions)
        digest = hashlib.sha256(survivor_path.read_bytes()).hexdigest()
        return number, versions, digest
    finally:
        os.close(survivor)
        os.close(anchor)


def check_file(path, owner, versions):
    fd = os.open(path, os.O_RDONLY)
    try:
        for sector, version in enumerate(versions):
            actual = os.pread(fd, SECTOR, sector * SECTOR)
            assert actual == payload(owner, sector, version), (
                path, sector, version, hashlib.sha256(actual).hexdigest())
    finally:
        os.close(fd)


def create(base, rounds=ROUNDS):
    base.mkdir()
    anchor = os.open(base / "anchor", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    write_initial(anchor, 0)
    os.fsync(anchor)
    os.close(anchor)
    with multiprocessing.Pool(WORKERS) as pool:
        results = pool.starmap(worker, (
            (base, number, rounds) for number in range(WORKERS)))
    os.sync()
    with (base / "results").open("w") as output:
        for number, versions, digest in sorted(results):
            output.write(f"{number} {digest} ")
            output.write(",".join(map(str, versions)) + "\n")
        output.flush()
        os.fsync(output.fileno())
    verify(base)
    print(f"orphan/ref stress passed: "
          f"{WORKERS * rounds * OPEN_ORPHANS} cleanups", flush=True)


def verify(base):
    check_file(base / "anchor", 0, [0] * SECTORS)
    lines = (base / "results").read_text().splitlines()
    assert len(lines) == WORKERS
    for line in lines:
        number_text, digest, versions_text = line.split()
        number = int(number_text)
        versions = list(map(int, versions_text.split(",")))
        path = base / f"survivor-{number}"
        check_file(path, number + 1, versions)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    assert {path.name for path in base.iterdir()} == {
        "anchor", "results", *(f"survivor-{i}" for i in range(WORKERS))}
    print("persisted orphan/ref byte models verified", flush=True)


if __name__ == "__main__":
    signal.alarm(1800)
    phase = sys.argv[1]
    directory = Path(sys.argv[2]).absolute()
    count = int(sys.argv[3]) if len(sys.argv) > 3 else ROUNDS
    globals()[phase](directory, count) if phase == "create" else verify(directory)
