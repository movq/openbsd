#!/usr/bin/env python3
"""Concurrent snapshot/COW stress with self-identifying sector contents."""
import hashlib
import multiprocessing
import os
from pathlib import Path
import random
import signal
import struct
import subprocess
import sys
import time


SECTOR = 4096
SECTORS = 256
WORKERS = 8
OPERATIONS = 2000
SNAPSHOTS = 32
MAGIC = b"BTRFSTAG"


def command(mount, operation, *paths, readonly=False):
    argv = ["btrfs", "subvolume", operation]
    if readonly:
        argv.append("-r")
    argv.extend([str(mount), *map(str, paths)])
    result = subprocess.run(argv, text=True, capture_output=True)
    assert result.returncode == 0, (argv, result.stdout, result.stderr)


def payload(worker, sector, version):
    header = MAGIC + struct.pack("<III", worker, sector, version)
    digest = hashlib.sha256(header).digest()
    pattern = header + digest
    return (pattern * ((SECTOR + len(pattern) - 1) // len(pattern)))[:SECTOR]


def operation_stream(worker, count):
    rng = random.Random(0x5A4F0000 + worker)
    for version in range(1, count + 1):
        yield version, rng.randrange(SECTORS)


def final_versions(worker, count):
    versions = [0] * SECTORS
    for version, sector in operation_stream(worker, count):
        versions[sector] = version
    return versions


def writer(source, worker, count, started):
    fd = os.open(source / f"file-{worker}", os.O_RDWR)
    try:
        started.release()
        for version, sector in operation_stream(worker, count):
            data = payload(worker, sector, version)
            assert os.pwrite(fd, data, sector * SECTOR) == SECTOR
            if version % 32 == 0:
                os.fsync(fd)
        os.fsync(fd)
    finally:
        os.close(fd)


def validate_file(path, worker, maximum=OPERATIONS, exact=None):
    fd = os.open(path, os.O_RDONLY)
    try:
        assert os.fstat(fd).st_size == SECTORS * SECTOR
        for sector in range(SECTORS):
            data = os.pread(fd, SECTOR, sector * SECTOR)
            assert len(data) == SECTOR, (path, sector, len(data))
            assert data[:len(MAGIC)] == MAGIC, (
                path, sector, data[:32].hex())
            found_worker, found_sector, version = struct.unpack_from(
                "<III", data, len(MAGIC))
            assert (found_worker, found_sector) == (worker, sector), (
                path, sector, found_worker, found_sector, version)
            assert 0 <= version <= maximum, (path, sector, version, maximum)
            if exact is not None:
                assert version == exact[sector], (
                    path, sector, version, exact[sector])
            assert data == payload(worker, sector, version), (
                path, sector, version,
                hashlib.sha256(data).hexdigest())
    finally:
        os.close(fd)


def validate_tree(path, maximum=OPERATIONS, exact=False):
    for worker in range(WORKERS):
        versions = final_versions(worker, maximum) if exact else None
        validate_file(path / f"file-{worker}", worker, maximum, versions)


def create(mount, count=OPERATIONS, snapshots=SNAPSHOTS):
    command(mount, "create", "source")
    source = mount / "source"
    for worker in range(WORKERS):
        with (source / f"file-{worker}").open("wb", buffering=0) as output:
            for sector in range(SECTORS):
                assert output.write(payload(worker, sector, 0)) == SECTOR
            os.fsync(output.fileno())
    os.sync()

    started = multiprocessing.Semaphore(0)
    children = [
        multiprocessing.Process(target=writer,
                                args=(source, worker, count, started))
        for worker in range(WORKERS)
    ]
    for child in children:
        child.start()
    for _ in children:
        assert started.acquire(timeout=10)

    retained = []
    try:
        for number in range(snapshots):
            name = f"snapshot-{number:03d}"
            command(mount, "snapshot", "source", name, readonly=True)
            retained.append(name)
            # Recheck old snapshots after later COW allocations and commits.
            for retained_name in retained:
                validate_tree(mount / retained_name, count)
            if len(retained) > 4:
                command(mount, "delete", retained.pop(0))
    finally:
        for child in children:
            child.join(120)
            if child.is_alive():
                child.kill()
                child.join()
                raise AssertionError(f"writer {child.pid} did not finish")
            assert child.exitcode == 0, (child.pid, child.exitcode)

    validate_tree(source, count, exact=True)
    command(mount, "snapshot", "source", "final", readonly=True)
    validate_tree(mount / "final", count, exact=True)
    command(mount, "delete", "source")
    for name in retained:
        validate_tree(mount / name, count)
    validate_tree(mount / "final", count, exact=True)
    (mount / "snapshot-stress-count").write_text(f"{count}\n")
    os.sync()
    print(f"snapshot stress passed: {WORKERS * count} writes, "
          f"{snapshots} snapshots", flush=True)


def verify(mount):
    count = int((mount / "snapshot-stress-count").read_text())
    snapshots = sorted(path for path in mount.glob("snapshot-*")
                       if path.is_dir())
    assert 1 <= len(snapshots) <= 4, [path.name for path in snapshots]
    for snapshot in snapshots:
        validate_tree(snapshot, count)
    validate_tree(mount / "final", count, exact=True)
    print(f"persisted snapshot COW data verified in "
          f"{len(snapshots) + 1} trees", flush=True)


if __name__ == "__main__":
    signal.alarm(900)
    phase = sys.argv[1]
    root = Path(sys.argv[2]).absolute()
    if phase == "create":
        count = int(sys.argv[3]) if len(sys.argv) > 3 else OPERATIONS
        snapshots = int(sys.argv[4]) if len(sys.argv) > 4 else SNAPSHOTS
        create(root, count, snapshots)
    else:
        verify(root)
