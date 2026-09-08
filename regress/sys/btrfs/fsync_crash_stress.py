#!/usr/bin/env python3
"""Concurrent append/fsync crash durability with host-visible epochs."""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import signal
import struct
import sys


MAGIC = b"BTRFSFSYNC"
RECORD_SIZE = 64 * 1024
WORKERS = 8
EPOCHS = 10000


def record(worker, epoch):
    header = MAGIC + struct.pack("<II", worker, epoch)
    seed = hashlib.sha256(header).digest()
    body = (seed * ((RECORD_SIZE - len(header) + len(seed) - 1) //
                    len(seed)))[:RECORD_SIZE - len(header)]
    return header + body


def digest(data):
    return hashlib.sha256(data).hexdigest()


def initialize(base):
    base.mkdir()
    for worker in range(WORKERS):
        path = base / f"worker-{worker}"
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
    os.sync()
    print(f"initialized {WORKERS} append logs", flush=True)


def writer(base, worker, epochs):
    fd = os.open(base / f"worker-{worker}", os.O_WRONLY | os.O_APPEND)
    try:
        for epoch in range(epochs):
            data = record(worker, epoch)
            offset = 0
            while offset < len(data):
                written = os.write(fd, data[offset:])
                assert written > 0
                offset += written
            os.fsync(fd)
            print(f"ACK {worker} {epoch} {digest(data)}", flush=True)
    finally:
        os.close(fd)


def write(base, epochs=EPOCHS):
    with concurrent.futures.ProcessPoolExecutor(
            max_workers=WORKERS) as executor:
        jobs = [executor.submit(writer, base, worker, epochs)
                for worker in range(WORKERS)]
        for job in jobs:
            job.result()


def read_acknowledgements(path):
    acknowledged = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) != 4 or fields[0] != "ACK":
            continue
        worker, epoch = map(int, fields[1:3])
        assert 0 <= worker < WORKERS
        assert epoch == acknowledged.get(worker, (-1, ""))[0] + 1, line
        acknowledged[worker] = epoch, fields[3]
    return acknowledged


def verify_file(path, worker, acknowledged):
    size = path.stat().st_size
    required = (acknowledged + 1) * RECORD_SIZE
    assert size >= required, (path, size, required, acknowledged)

    fd = os.open(path, os.O_RDONLY)
    try:
        complete = size // RECORD_SIZE
        for epoch in range(complete):
            data = os.pread(fd, RECORD_SIZE, epoch * RECORD_SIZE)
            assert len(data) == RECORD_SIZE, (path, epoch, len(data))
            expected = record(worker, epoch)
            assert data == expected, (
                path, epoch, digest(data), digest(expected))

        tail = size % RECORD_SIZE
        if tail:
            data = os.pread(fd, tail, complete * RECORD_SIZE)
            expected = record(worker, complete)[:tail]
            assert data == expected, (
                path, complete, tail, digest(data), digest(expected))
    finally:
        os.close(fd)


def verify(base, acknowledgement_path):
    acknowledgements = read_acknowledgements(acknowledgement_path)
    assert len(acknowledgements) == WORKERS, acknowledgements
    for worker in range(WORKERS):
        epoch, expected_digest = acknowledgements[worker]
        expected = record(worker, epoch)
        assert digest(expected) == expected_digest, (
            worker, epoch, expected_digest)
        verify_file(base / f"worker-{worker}", worker, epoch)
    print("all host-acknowledged fsync epochs survived", flush=True)


if __name__ == "__main__":
    signal.alarm(1800)
    phase = sys.argv[1]
    directory = Path(sys.argv[2]).absolute()
    if phase == "initialize":
        initialize(directory)
    elif phase == "write":
        count = int(sys.argv[3]) if len(sys.argv) > 3 else EPOCHS
        write(directory, count)
    elif phase == "verify":
        verify(directory, Path(sys.argv[3]).absolute())
    else:
        raise ValueError(phase)
