#!/usr/bin/env python3
"""Grow existing data capacity, verify durable mappings, and reclaim data."""
import errno
import os
from pathlib import Path
import sys

from namespace import child_checks, wait
from unlink import sync


def payload(worker, sector):
    return (f"{worker}:{sector:08d}\n".encode() * 4096)[:4096]


def write_file(path, worker, count):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        for sector in range(count):
            data = payload(worker, sector)
            assert os.write(fd, data) == len(data)
            if sector % 128 == 127:
                os.fsync(fd)
        os.fsync(fd)
    finally:
        os.close(fd)


def create(base):
    base.mkdir()
    before = os.statvfs(base)
    (base / "initial-blocks").write_text(str(before.f_blocks))
    # DUP mkfs fixtures can start with much larger data groups.
    count = max(3072, before.f_bavail // 4 + 1024)
    (base / "sector-count").write_text(str(count))
    children = [
        child_checks(lambda w=w: write_file(base / str(w), w, count))
        for w in range(4)
    ]
    for pid in children:
        wait(pid)
    sync(base)
    assert os.statvfs(base).f_blocks > before.f_blocks
    verify(base)
    print("concurrent data chunk growth passed", flush=True)


def verify(base):
    count = int((base / "sector-count").read_text())
    for worker in range(4):
        with (base / str(worker)).open("rb") as stream:
            for sector in range(count):
                assert stream.read(4096) == payload(worker, sector)
            assert stream.read() == b""
    assert os.statvfs(base).f_blocks > int(
        (base / "initial-blocks").read_text())
    print("grown chunk data verified", flush=True)


def capacity(base):
    base.mkdir()
    before = os.statvfs(base)
    count = 0
    fd = os.open(base / "full", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        while True:
            try:
                assert os.write(fd, payload(9, count)) == 4096
            except OSError as error:
                assert error.errno == errno.ENOSPC, error
                break
            count += 1
            if count % 128 == 0:
                os.fsync(fd)
        os.fsync(fd)
        assert os.fstat(fd).st_size == count * 4096
        after = os.statvfs(base)
        assert after.f_blocks > before.f_blocks
        assert after.f_bavail == 0, ("metadata exhausted first", after)
        assert not after.f_flag & os.ST_RDONLY
        for sector in range(count):
            assert os.pread(fd, 4096, sector * 4096) == payload(9, sector)
    finally:
        os.close(fd)
    # Bounded orphan cleanup can reap the fragmented file at capacity.
    os.unlink(base / "full")
    sync(base)
    assert os.statvfs(base).f_bavail >= count
    write_file(base / "reuse", 8, 512)
    sync(base)
    print(f"device capacity and reuse passed after {count} sectors", flush=True)


def hold(base):
    import time
    create(base)
    Path("/tmp/btrfs-chunks-ready").write_text("ready\n")
    while True:
        time.sleep(60)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).resolve())
