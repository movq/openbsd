#!/usr/bin/env python3
"""Deterministic concurrent mixed-I/O stress with an independent byte model."""
import concurrent.futures
import fcntl
import hashlib
import os
from pathlib import Path
import random
import signal
import struct
import sys
import time


SECTOR = 4096
SOURCE_SIZE = 8 * 1024 * 1024
MAX_SIZE = 12 * 1024 * 1024
WORKERS = 8
OPERATIONS = 1200


def source_payload():
    return b"".join(
        hashlib.sha256(struct.pack("<Q", sector)).digest() * (SECTOR // 32)
        for sector in range(SOURCE_SIZE // SECTOR)
    )


def write_payload(worker, operation, length):
    digest = hashlib.sha256(struct.pack("<II", worker, operation)).digest()
    return (digest * ((length + len(digest) - 1) // len(digest)))[:length]


def clone(source, destination, offset, length, target):
    control = os.open("/dev/btrfs-control", os.O_RDWR)
    try:
        fcntl.ioctl(control, 0x8020420b, struct.pack(
            "=iiQQQ", source, destination, offset, target, length))
    finally:
        os.close(control)


def operation_stream(worker, count):
    rng = random.Random(0xB7F50000 + worker)
    for operation in range(count):
        choice = rng.randrange(100)
        if choice < 58:
            offset = rng.randrange(MAX_SIZE - 16 * SECTOR)
            length = rng.randrange(1, 16 * SECTOR + 1)
            yield operation, "write", offset, length
        elif choice < 72:
            yield operation, "truncate", rng.randrange(MAX_SIZE + 1), 0
        elif choice < 85:
            length = rng.randrange(1, 17) * SECTOR
            source = rng.randrange(0, SOURCE_SIZE - length + 1, SECTOR)
            target = rng.randrange(0, MAX_SIZE - length + 1, SECTOR)
            yield operation, "clone", source, length, target
        elif choice < 91:
            yield operation, "fsync", 0, 0
        elif choice < 96:
            yield operation, "rename", 0, 0
        else:
            offset = rng.randrange(MAX_SIZE)
            length = rng.randrange(1, 8 * SECTOR + 1)
            yield operation, "read", offset, length


def resize(model, size):
    if size < len(model):
        del model[size:]
    elif size > len(model):
        model.extend(bytes(size - len(model)))


def grow(model, size):
    if size > len(model):
        model.extend(bytes(size - len(model)))


def apply_model(model, source, worker, operation):
    number, kind, first, second, *rest = operation
    if kind == "write":
        grow(model, first + second)
        model[first:first + second] = write_payload(worker, number, second)
    elif kind == "truncate":
        resize(model, first)
    elif kind == "clone":
        target = rest[0]
        grow(model, target + second)
        model[target:target + second] = source[first:first + second]


def check_range(fd, model, rng):
    if not model:
        return
    offset = rng.randrange(len(model))
    length = min(rng.randrange(1, 4 * SECTOR + 1), len(model) - offset)
    actual = os.pread(fd, length, offset)
    assert actual == model[offset:offset + length], (
        offset, length, hashlib.sha256(actual).hexdigest(),
        hashlib.sha256(model[offset:offset + length]).hexdigest())


def worker(base, worker_number, count):
    source_data = source_payload()
    source_fd = os.open(base / "source", os.O_RDONLY)
    primary = base / f"file-{worker_number}"
    alternate = base / f"moved-{worker_number}"
    fd = os.open(primary, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    model = bytearray()
    current, other = primary, alternate
    check_rng = random.Random(0xC0FFEE00 + worker_number)
    try:
        for operation in operation_stream(worker_number, count):
            number, kind, first, second, *rest = operation
            if kind == "write":
                data = write_payload(worker_number, number, second)
                assert os.pwrite(fd, data, first) == len(data)
            elif kind == "truncate":
                os.ftruncate(fd, first)
            elif kind == "clone":
                clone(source_fd, fd, first, second, rest[0])
            elif kind == "fsync":
                os.fsync(fd)
            elif kind == "rename":
                os.rename(current, other)
                current, other = other, current
            elif kind == "read":
                actual = os.pread(fd, second, first)
                assert actual == model[first:first + second], (
                    worker_number, number, kind, first, second)
            apply_model(model, source_data, worker_number, operation)
            if number % 31 == 30:
                check_range(fd, model, check_rng)
        if current != primary:
            os.rename(current, primary)
        os.fsync(fd)
        actual = hashlib.sha256()
        offset = 0
        while data := os.pread(fd, 1024 * 1024, offset):
            actual.update(data)
            offset += len(data)
        expected = hashlib.sha256(model).hexdigest()
        assert actual.hexdigest() == expected, (
            worker_number, len(model), actual.hexdigest(), expected)
        return worker_number, len(model), expected
    finally:
        os.close(fd)
        os.close(source_fd)


def create(base, count=OPERATIONS):
    base.mkdir()
    source = source_payload()
    (base / "source").write_bytes(source)
    os.sync()
    with concurrent.futures.ProcessPoolExecutor(
            max_workers=WORKERS) as executor:
        futures = [executor.submit(worker, base, number, count)
                   for number in range(WORKERS)]
        results = [future.result() for future in futures]
    os.sync()
    (base / "results").write_text(
        "".join(f"{worker} {size} {digest}\n"
                for worker, size, digest in sorted(results)))
    os.sync()
    verify(base, count)
    print(f"concurrent integrity stress passed: {WORKERS * count} operations",
          flush=True)


def verify(base, count=OPERATIONS):
    source = source_payload()
    assert (base / "source").read_bytes() == source
    recorded = {}
    for line in (base / "results").read_text().splitlines():
        worker_number, size, digest = line.split()
        recorded[int(worker_number)] = int(size), digest
    assert len(recorded) == WORKERS
    for worker_number in range(WORKERS):
        model = bytearray()
        for operation in operation_stream(worker_number, count):
            apply_model(model, source, worker_number, operation)
        path = base / f"file-{worker_number}"
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        expected = hashlib.sha256(model).hexdigest()
        assert (path.stat().st_size, actual) == (len(model), expected), (
            worker_number, path.stat().st_size, len(model), actual, expected)
        assert recorded[worker_number] == (len(model), expected)
        assert not (base / f"moved-{worker_number}").exists()
    print("persisted concurrent byte models verified", flush=True)


if __name__ == "__main__":
    signal.alarm(900)
    phase = sys.argv[1]
    directory = Path(sys.argv[2]).absolute()
    operations = int(sys.argv[3]) if len(sys.argv) > 3 else OPERATIONS
    globals()[phase](directory, operations)
