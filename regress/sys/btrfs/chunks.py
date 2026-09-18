#!/usr/bin/env python3
"""Grow existing data capacity, verify durable mappings, and reclaim data."""
import argparse
import errno
import os
from pathlib import Path

from namespace import child_checks, wait
from unlink import sync

WRITE_SECTORS = 16
FSYNC_SECTORS = 1024


def payload(worker, sector):
    return (f"{worker}:{sector:08d}\n".encode() * 4096)[:4096]


def write_file(path, worker, count):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        for sector in range(0, count, WRITE_SECTORS):
            end = min(sector + WRITE_SECTORS, count)
            data = b"".join(payload(worker, s) for s in range(sector, end))
            assert os.write(fd, data) == len(data)
            if end % FSYNC_SECTORS == 0:
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
    # Cleanup can finish in a later generation than the directory unlink.
    # Publish that generation too before checking that all pins are free.
    os.sync()
    reclaimed = os.statvfs(base)
    assert reclaimed.f_bavail >= count, (count, reclaimed)
    write_file(base / "reuse", 8, 512)
    sync(base)
    print(f"device capacity and reuse passed after {count} sectors", flush=True)


def metadata(base):
    import enospc
    before = os.statvfs(base.parent).f_blocks
    enospc.main(str(base))
    assert os.statvfs(base).f_blocks > before
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    print("metadata growth to device exhaustion passed", flush=True)


def metadata_verify(base):
    groups = sorted(base.iterdir())
    assert groups
    for i, group in enumerate(groups):
        assert group.name == f"group-{i:04d}"
        names = sorted(group.iterdir())
        assert len(names) == 128 or i == len(groups) - 1
        for j, name in enumerate(names):
            assert name.name == f"{j:04d}-" + "x" * 240
            assert name.stat().st_size == 0 and name.stat().st_nlink == 1
    print("grown metadata namespace verified", flush=True)


def combined(base):
    def names(worker):
        root = base / f"names-{worker}"
        root.mkdir()
        for group in range(64):
            directory = root / str(group)
            directory.mkdir()
            for i in range(128):
                (directory / (f"{i:04d}-" + "x" * 240)).touch()
        sync(root)

    base.mkdir()
    before = os.statvfs(base).f_blocks
    children = [child_checks(lambda w=w: names(w)) for w in range(2)]
    children += [child_checks(lambda w=w: write_file(base / str(w), w, 3072))
                 for w in range(4)]
    for pid in children:
        wait(pid)
    (base / "initial-blocks").write_text(str(before))
    (base / "sector-count").write_text("3072")
    sync(base)
    combined_verify(base)


def combined_verify(base):
    verify(base)
    for worker in range(2):
        for group in range(64):
            directory = base / f"names-{worker}" / str(group)
            assert len(list(directory.iterdir())) == 128
            for i in range(128):
                assert (directory / (f"{i:04d}-" + "x" * 240)).stat().st_size == 0
    print("simultaneous metadata and data growth passed", flush=True)


def hold(base):
    import time
    create(base)
    Path("/tmp/btrfs-chunks-ready").write_text("ready\n")
    while True:
        time.sleep(60)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=[
        "create", "verify", "capacity", "metadata", "metadata_verify",
        "combined", "combined_verify", "hold"])
    parser.add_argument("directory", type=Path)
    parser.add_argument("--stress", action="store_true",
                        help="write 4 KiB at a time and fsync every 512 KiB "
                             "(default: 64 KiB writes, fsync every 4 MiB)")
    args = parser.parse_args()
    if args.stress:
        WRITE_SECTORS, FSYNC_SECTORS = 1, 128
    globals()[args.phase](args.directory.resolve())
