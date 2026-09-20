#!/usr/bin/env python3
"""Use several new data chunks before publishing their common generation."""
import os
from pathlib import Path
import sys

BLOCK = 65536


def payload(number):
    return number.to_bytes(8, "little") * (BLOCK // 8)


def create(base):
    base.mkdir()
    (base / "anchor").write_bytes(b"committed anchor\n")
    (base / "data").touch()
    (base / "blocks").write_text(str(os.statvfs(base).f_blocks))


def mutate(base, durable=True, retire=False):
    before = os.statvfs(base).f_blocks
    capacity, growths = before, 0
    with (base / "data").open("r+b", buffering=0) as stream:
        for number in range(16384):
            assert stream.write(payload(number)) == BLOCK
            current = os.statvfs(base).f_blocks
            if current > capacity:
                growths += 1
                capacity = current
                if retire and growths == 1:
                    # This allocation belongs to a new group and will be
                    # drained before the later additions.
                    (base / "victim").write_bytes(b"retired\n" * 8192)
            if growths == 3:
                break
        else:
            raise AssertionError("data capacity did not grow three times")
        # This spans retired payloads in new groups as well as ordered data.
        verify(base)
        if retire:
            (base / "victim").unlink()
            # Materialize the drop without requiring more data capacity.
            # Aborting afterwards must release exclusions before groups.
            for number in range(4096):
                (base / f"entry-{number}").touch()
        if durable:
            os.fsync(stream.fileno())
    verify(base)


def mutate_retire(base):
    mutate(base, retire=True)


def pending(base):
    mutate(base, durable=False)


def pending_retire(base):
    mutate(base, durable=False, retire=True)


def verify_original(base):
    assert (base / "anchor").read_bytes() == b"committed anchor\n"
    assert (base / "data").stat().st_size == 0
    assert os.statvfs(base).f_blocks == int((base / "blocks").read_text())
    print("unpublished growth and data rolled back", flush=True)


def verify(base):
    assert (base / "anchor").read_bytes() == b"committed anchor\n"
    assert os.statvfs(base).f_blocks > int((base / "blocks").read_text())
    size = (base / "data").stat().st_size
    assert size > 0 and size % BLOCK == 0
    with (base / "data").open("rb") as stream:
        for number in range(size // BLOCK):
            assert stream.read(BLOCK) == payload(number), number
        assert stream.read() == b""
    print("grown capacity and file data verified", flush=True)


def fill(base):
    (base / "length").write_text("0")
    space = os.statvfs(base)
    count = space.f_bavail * space.f_frsize // BLOCK
    assert count > 0
    (base / "length").write_text(str(count))
    with (base / "data").open("r+b", buffering=0) as stream:
        for number in range(count):
            assert stream.write(payload(number)) == BLOCK
    assert os.statvfs(base).f_blocks == space.f_blocks


def reuse(base):
    before = os.statvfs(base).f_blocks
    count = int((base / "length").read_text())
    (base / "data").unlink()
    with (base / "data").open("xb", buffering=0) as stream:
        for number in range(count):
            assert stream.write(payload(number + 12345)) == BLOCK
        os.fsync(stream.fileno())
    assert os.statvfs(base).f_blocks == before
    verify_reused(base)


def verify_reused(base):
    assert (base / "anchor").read_bytes() == b"committed anchor\n"
    assert os.statvfs(base).f_blocks == int((base / "blocks").read_text())
    count = int((base / "length").read_text())
    with (base / "data").open("rb") as stream:
        for number in range(count):
            assert stream.read(BLOCK) == payload(number + 12345), number
        assert stream.read() == b""
    print("retired data reclaimed without chunk growth", flush=True)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
