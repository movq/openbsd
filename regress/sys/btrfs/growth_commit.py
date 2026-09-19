#!/usr/bin/env python3
"""Publish pending file data and a new data chunk in the same generation."""
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


def mutate(base):
    before = os.statvfs(base).f_blocks
    with (base / "data").open("r+b", buffering=0) as stream:
        for number in range(16384):
            assert stream.write(payload(number)) == BLOCK
            if os.statvfs(base).f_blocks > before:
                break
        else:
            raise AssertionError("data capacity did not grow")
        os.fsync(stream.fileno())
    verify(base)


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
    print("published growth and pending data verified", flush=True)


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
