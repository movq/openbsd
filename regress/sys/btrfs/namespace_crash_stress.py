#!/usr/bin/env python3
"""Concurrent namespace-fsync crash durability with host-visible epochs."""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import signal
import struct
import sys


MAGIC = b"BTRFSNAMESPACE"
PAYLOAD_SIZE = 16 * 1024
WORKERS = 8
EPOCHS = 10000


def payload(worker, epoch):
    header = MAGIC + struct.pack("<II", worker, epoch)
    seed = hashlib.sha256(header).digest()
    body = (seed * ((PAYLOAD_SIZE - len(header) + len(seed) - 1) //
                    len(seed)))[:PAYLOAD_SIZE - len(header)]
    return header + body


def digest(data):
    return hashlib.sha256(data).hexdigest()


def initialize(base):
    base.mkdir()
    (base / "left").mkdir()
    (base / "right").mkdir()
    os.sync()
    print("initialized namespace directories", flush=True)


def writer(base, worker, epochs):
    left = base / "left"
    right = base / "right"
    left_fd = os.open(left, os.O_RDONLY)
    right_fd = os.open(right, os.O_RDONLY)
    try:
        for epoch in range(epochs):
            temporary = left / f"temporary-{worker}-{epoch}"
            renamed = right / f"renamed-{worker}-{epoch}"
            committed = left / f"committed-{worker}-{epoch}"
            data = payload(worker, epoch)

            fd = os.open(temporary,
                         os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                offset = 0
                while offset < len(data):
                    written = os.write(fd, data[offset:])
                    assert written > 0
                    offset += written
                os.fsync(fd)
            finally:
                os.close(fd)

            os.rename(temporary, renamed)
            os.link(renamed, committed)
            os.unlink(renamed)
            os.fsync(left_fd)
            os.fsync(right_fd)
            print(f"ACK {worker} {epoch} {digest(data)}", flush=True)
    finally:
        os.close(right_fd)
        os.close(left_fd)


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


def parse_name(path):
    fields = path.name.split("-")
    assert len(fields) == 3, path
    kind, worker, epoch = fields
    assert kind in ("temporary", "renamed", "committed"), path
    worker, epoch = int(worker), int(epoch)
    assert 0 <= worker < WORKERS and epoch >= 0, path
    return kind, worker, epoch


def verify_payload(path, worker, epoch):
    data = path.read_bytes()
    expected = payload(worker, epoch)
    assert data == expected, (
        path, len(data), digest(data), digest(expected))


def verify(base, acknowledgement_path):
    acknowledgements = read_acknowledgements(acknowledgement_path)
    assert len(acknowledgements) == WORKERS, acknowledgements

    recovered = {}
    for directory, allowed in ((base / "left", {"temporary", "committed"}),
                               (base / "right", {"renamed"})):
        for path in directory.iterdir():
            kind, worker, epoch = parse_name(path)
            assert kind in allowed, path
            verify_payload(path, worker, epoch)
            key = worker, epoch
            kinds = recovered.setdefault(key, set())
            assert kind not in kinds, path
            kinds.add(kind)

    for worker in range(WORKERS):
        latest, expected_digest = acknowledgements[worker]
        assert digest(payload(worker, latest)) == expected_digest
        for epoch in range(latest + 1):
            assert recovered.get((worker, epoch)) == {"committed"}, (
                worker, epoch, recovered.get((worker, epoch)))
    print("all host-acknowledged namespace epochs survived", flush=True)


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
