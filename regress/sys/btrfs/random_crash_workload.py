#!/usr/bin/env python3
"""Guest workload and durability oracle for random_crash.py."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import random
import sys
import threading
import traceback


RECORD_SIZE = 65536


def data(seed, worker, epoch, operation, size):
    tag = f"{seed}:{worker}:{epoch}:{operation}".encode()
    return hashlib.shake_256(tag).digest(size)


def edits(seed, worker, epoch):
    rng = random.Random(f"{seed}:{worker}:{epoch}")
    sizes = (0, 1, 4095, 4096, 4097, 16384, 65536, 131073)
    for operation in range(12):
        if rng.randrange(3) == 0:
            yield "truncate", rng.choice(sizes), b""
        else:
            offset = rng.choice(sizes)
            size = rng.choice(sizes[1:])
            yield "write", offset, data(seed, worker, epoch, operation, size)


def expected_file(seed, worker, epoch):
    result = bytearray()
    for kind, offset, payload in edits(seed, worker, epoch):
        end = offset if kind == "truncate" else offset + len(payload)
        if end > len(result):
            result.extend(b"\0" * (end - len(result)))
        if kind == "truncate":
            del result[end:]
        else:
            result[offset:end] = payload
    return bytes(result)


def write_all(fd, payload, offset):
    while payload:
        count = os.pwrite(fd, payload, offset)
        if count <= 0:
            raise RuntimeError("short write without progress")
        payload = payload[count:]
        offset += count


def acknowledge(worker, epoch):
    # A single short write keeps messages from concurrent workers separate.
    message = f"ACK {worker} {epoch}\n".encode()
    if os.write(1, message) != len(message):
        raise RuntimeError("short acknowledgement write")


def kind_for(mode, worker):
    return "append" if mode == "append" or (
        mode == "mixed" and worker % 2 == 0) else "namespace"


def initialize(base, workers):
    base.mkdir()
    for worker in range(workers):
        directory = base / str(worker)
        directory.mkdir()
        (directory / "left").mkdir()
        (directory / "right").mkdir()
        (directory / "append").touch()
    os.sync()


def writer(base, seed, worker, mode, barrier):
    directory = base / str(worker)
    append = os.open(directory / "append", os.O_RDWR)
    left = directory / "left"
    right = directory / "right"
    left_fd = os.open(left, os.O_RDONLY)
    right_fd = os.open(right, os.O_RDONLY)
    try:
        barrier.wait()
        epoch = 0
        while True:
            if kind_for(mode, worker) == "append":
                payload = data(seed, worker, epoch, 0, RECORD_SIZE)
                write_all(append, payload, epoch * RECORD_SIZE)
                os.fsync(append)
            else:
                temporary = left / f"temporary-{epoch}"
                renamed = right / f"renamed-{epoch}"
                committed = left / f"committed-{epoch}"
                fd = os.open(temporary, os.O_CREAT | os.O_EXCL | os.O_RDWR,
                             0o600)
                try:
                    for operation, (kind, offset, payload) in enumerate(
                            edits(seed, worker, epoch)):
                        if kind == "truncate":
                            os.ftruncate(fd, offset)
                        else:
                            write_all(fd, payload, offset)
                        # Retire and replace allocations published in a log,
                        # including non-sector-aligned truncation and holes.
                        if operation % 4 == 0:
                            os.fsync(fd)
                    os.fsync(fd)
                finally:
                    os.close(fd)
                os.rename(temporary, renamed)
                os.link(renamed, committed)
                os.unlink(renamed)
                os.fsync(left_fd)
                os.fsync(right_fd)
            acknowledge(worker, epoch)
            epoch += 1
    finally:
        os.close(right_fd)
        os.close(left_fd)
        os.close(append)


def write(base, seed, workers, mode):
    barrier = threading.Barrier(
        workers + 1, action=lambda: print("READY", flush=True))

    def guarded_writer(worker):
        try:
            writer(base, seed, worker, mode, barrier)
        except BaseException:
            # Other workers run indefinitely; don't hide an I/O failure by
            # waiting for their futures before reporting this one's exception.
            traceback.print_exc()
            os._exit(1)

    with ThreadPoolExecutor(max_workers=workers) as executor:
        jobs = [executor.submit(guarded_writer, worker)
                for worker in range(workers)]
        # The host starts its crash timer only after the guest is ready.
        barrier.wait(timeout=30)
        for job in jobs:
            job.result()


def acknowledgements(text, workers):
    latest = [-1] * workers
    ready = False
    for line in text.splitlines():
        if line == "READY":
            if ready:
                raise ValueError("duplicate READY")
            ready = True
            continue
        fields = line.split()
        if len(fields) != 3 or fields[0] != "ACK" or not ready:
            raise ValueError(f"invalid acknowledgement: {line!r}")
        worker, epoch = map(int, fields[1:])
        if not 0 <= worker < workers or epoch != latest[worker] + 1:
            raise ValueError(f"out-of-order acknowledgement: {line!r}")
        latest[worker] = epoch
    if not ready:
        raise ValueError("workload never became ready")
    return latest


def verify(base, seed, workers, mode, text):
    latest = acknowledgements(text, workers)
    for worker, last in enumerate(latest):
        directory = base / str(worker)
        if kind_for(mode, worker) == "append":
            with (directory / "append").open("rb") as stream:
                for epoch in range(last + 1):
                    actual = stream.read(RECORD_SIZE)
                    expected = data(seed, worker, epoch, 0, RECORD_SIZE)
                    if actual != expected:
                        raise ValueError(
                            f"append mismatch: worker={worker} epoch={epoch}")
        else:
            for epoch in range(last + 1):
                path = directory / "left" / f"committed-{epoch}"
                if path.read_bytes() != expected_file(seed, worker, epoch):
                    raise ValueError(f"namespace data mismatch: {path}")
                if path.stat().st_nlink != 1:
                    raise ValueError(f"wrong link count: {path}")
                for parent, name in (("left", "temporary"),
                                     ("right", "renamed")):
                    if os.path.lexists(directory / parent / f"{name}-{epoch}"):
                        raise ValueError(f"removed name survived: {worker}/{epoch}")
    # Unacknowledged writes may be absent, partial, or fully durable.
    print(json.dumps({"verified_epochs": [epoch + 1 for epoch in latest]}),
          flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("initialize", "write", "verify"))
    parser.add_argument("directory", type=Path)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--mode", choices=("append", "namespace", "mixed"),
                        default="mixed")
    args = parser.parse_args()
    if args.workers < 1:
        parser.error("--workers must be positive")
    if args.phase == "initialize":
        initialize(args.directory, args.workers)
    elif args.phase == "write":
        write(args.directory, args.seed, args.workers, args.mode)
    else:
        verify(args.directory, args.seed, args.workers, args.mode,
               sys.stdin.read())


if __name__ == "__main__":
    main()
