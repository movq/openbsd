#!/usr/bin/env python3
"""Reassign empty groups between data and metadata on a small device."""
import errno
import json
import os
from pathlib import Path
import re
import select
import sys

import enospc
from checksums import payload, SECTOR
from namespace import child_checks, wait
from mirrors import inspect


def fill(path):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    count = 0
    try:
        for sector in range(131072):
            try:
                assert os.write(fd, payload(8, sector)) == SECTOR
            except OSError as error:
                assert error.errno == errno.ENOSPC, error
                break
            count += 1
            if count % 128 == 0:
                os.fsync(fd)
        else:
            raise AssertionError("use a small disposable image")
        os.fsync(fd)
        assert count > 512
        # Populate physical buffers, including interior sectors of chunks,
        # before releasing this allocation for metadata use.
        os.ftruncate(fd, (count - 1) * SECTOR)
        os.fsync(fd)
        for sector in range(count - 1):
            assert os.pread(fd, SECTOR, sector * SECTOR) == payload(8, sector)
    finally:
        os.close(fd)
    assert not os.statvfs(path).f_flag & os.ST_RDONLY
    print(f"filled and read {count - 1} data sectors", flush=True)


def prepare(base):
    base.mkdir()
    fill(base / "data")


def free_data(base):
    # prepare is followed by a remount for the host chunk snapshot. Populate
    # physical read windows again in the mount that reassigns the allocation.
    with (base / "data").open("rb") as stream:
        count = (base / "data").stat().st_size // SECTOR
        for sector in range(count):
            assert stream.read(SECTOR) == payload(8, sector)
    os.truncate(base / "data", 0)
    os.sync()
    assert (base / "data").stat().st_blocks == 0
    print("data groups emptied", flush=True)


def metadata(base, bounded=False):
    # Statfs traverses the group index while commit replaces and frees it.
    readfd, writefd = os.pipe()

    def sample():
        os.close(writefd)
        while not select.select([readfd], [], [], 0)[0]:
            st = os.statvfs(base)
            assert st.f_bavail <= st.f_bfree <= st.f_blocks
        os.close(readfd)

    pid = child_checks(sample)
    initial = os.statvfs(base)
    before = initial.f_bavail
    try:
        if bounded:
            root = base / "metadata"
            root.mkdir(exist_ok=True)
            # Leave enough ordinary reservation space for namespace removal.
            # Stop after observing returned data capacity, while the new
            # metadata capacity still has headroom for namespace removal.
            for group in range(len(list(root.iterdir())), 1024):
                directory = root / f"group-{group:04d}"
                directory.mkdir()
                for number in range(128):
                    (directory / (f"{number:04d}-" + "x" * 240)).touch()
                if group % 8 == 7:
                    os.sync()
                    print(f"created {(group + 1) * 128} files", flush=True)
                current = os.statvfs(base)
                if (before - current.f_bavail >= 512 and
                        current.f_blocks - current.f_bavail >
                        initial.f_blocks - initial.f_bavail):
                    os.sync()
                    break
            else:
                raise AssertionError("no data groups returned")
        else:
            enospc.main(str(base / "metadata"))
        assert os.statvfs(base).f_bavail < before
    finally:
        os.close(readfd)
        os.close(writefd)
        wait(pid)
    verify_metadata(base)


def metadata_bounded(base):
    metadata(base, bounded=True)


def verify_metadata(base):
    assert (base / "data").read_bytes() == b""
    count = 0
    for directory in (base / "metadata").iterdir():
        for path in directory.iterdir():
            assert path.stat().st_size == 0
            count += 1
    assert count > 0
    print(f"metadata namespace verified: {count} files", flush=True)


def free_metadata(base):
    count = 0
    for directory in sorted((base / "metadata").iterdir()):
        for path in directory.iterdir():
            path.unlink()
            count += 1
            if count % 2048 == 0:
                print(f"removed {count} files", flush=True)
        directory.rmdir()
    (base / "metadata").rmdir()
    os.sync()
    print("metadata groups emptied", flush=True)


def data_again(base):
    fill(base / "again")


def verify_data(base):
    with (base / "again").open("rb") as stream:
        count = (base / "again").stat().st_size // SECTOR
        for sector in range(count):
            assert stream.read(SECTOR) == payload(8, sector)
        assert stream.read() == b""
    print(f"reassigned data verified: {count} sectors", flush=True)


def chunks(image):
    result = []
    for item in inspect(str(image), "dump-tree", "-t", "chunk").split("\titem "):
        key = re.search(r"key \(\S+ CHUNK_ITEM (\d+)\)", item)
        if key is None:
            continue
        result.append(dict(
            logical=int(key[1]),
            length=int(re.search(r"\blength (\d+)", item)[1]),
            type=re.search(r"\btype (\S+)", item)[1],
            physical=[int(p) for p in re.findall(
                r"stripe \d+ devid \d+ offset (\d+)", item)]))
    return result


def snapshot(image, output):
    output.write_text(json.dumps(chunks(image)))


def reassigned(image, snapshot_path, oldtype, newtype):
    old = json.loads(snapshot_path.read_text())
    new = chunks(image)
    oldend = max(c["logical"] + c["length"] for c in old)
    removed = [c for c in old if c["type"].startswith(oldtype + "|") and
               not any(n["logical"] == c["logical"] for n in new)]
    assert removed, ("no empty groups returned", oldtype)
    reused = []
    for c in new:
        if not c["type"].startswith(newtype + "|"):
            continue
        if any(start < physical + c["length"] and
               physical < start + r["length"]
               for r in removed for start in r["physical"]
               for physical in c["physical"]):
            assert c["logical"] >= oldend, "reused a retired logical address"
            reused.append(c)
    assert reused, ("freed stripes were not reassigned", oldtype, newtype)
    assert any(c["type"].startswith(oldtype + "|") for c in new)
    print(f"{len(removed)} {oldtype} groups returned; "
          f"{len(reused)} {newtype} groups reuse their stripes")


if __name__ == "__main__":
    phase, path, *args = sys.argv[1:]
    if phase == "snapshot":
        snapshot(Path(path), Path(args[0]))
    elif phase == "reassigned":
        reassigned(Path(path), Path(args[0]), *args[1:])
    else:
        globals()[phase](Path(path).absolute())
