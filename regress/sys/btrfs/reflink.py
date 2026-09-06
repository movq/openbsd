#!/usr/bin/env python3
"""Range sharing, COW isolation and independent on-disk allocation checks."""
import errno
import fcntl
import concurrent.futures
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys

from mirrors import inspect
from send_metadata import fallocate

SECTOR = 4096
SIZE = 1024 * 1024


def clone(src, dst, offset=0, length=SIZE, target=0):
    ctl = os.open("/dev/btrfs-control", os.O_RDWR)
    try:
        fcntl.ioctl(ctl, 0x8020420b,
                    struct.pack("=iiQQQ", src, dst, offset, target, length))
    finally:
        os.close(ctl)


def link_range(src, dst, offset=0, length=SIZE, target=0):
    with src.open("rb") as a, dst.open("r+b") as b:
        clone(a.fileno(), b.fileno(), offset, length, target)


def payload():
    return b"".join(bytes([i % 251]) * SECTOR for i in range(SIZE // SECTOR))


def create(base):
    base.mkdir()
    data = payload()
    (base / "source").write_bytes(data)
    (base / "copy").touch()
    # Source data is still ordered; a later write must COW, not update the
    # allocation that has just acquired a second owner.
    link_range(base / "source", base / "copy")
    assert (base / "copy").read_bytes() == data
    (base / "split").write_bytes(b"X" * SIZE)
    with (base / "split").open("r+b") as f:
        assert f.read() == b"X" * SIZE  # Populate destination buffer cache.
    link_range(base / "source", base / "split", 3 * SECTOR,
               11 * SECTOR, 5 * SECTOR)
    # Share a nonzero source allocation offset at destination offset zero:
    # this also exercises unsigned wraparound in file-base backreferences.
    (base / "slice").touch()
    link_range(base / "source", base / "slice", 3 * SECTOR, 9 * SECTOR)
    os.link(base / "source", base / "alias")
    link_range(base / "source", base / "alias", 0, 2 * SECTOR, SIZE)
    (base / "tail").write_bytes(b"T" * (SECTOR + 37))
    (base / "tailcopy").touch()
    link_range(base / "tail", base / "tailcopy", 0, SECTOR + 37)
    (base / "gap").write_bytes(b"G" * 37)
    link_range(base / "source", base / "gap", 0, SECTOR, 4 * SECTOR)
    with (base / "hole").open("wb") as f:
        f.truncate(SIZE)
    (base / "zero").write_bytes(data)
    link_range(base / "hole", base / "zero", SECTOR, 7 * SECTOR, 3 * SECTOR)
    (base / "empty").touch()
    with (base / "source").open("rb") as a, (base / "empty").open("r+b") as b:
        for offset, length, target in ((1, SECTOR, 0), (0, SECTOR, 1),
                                       (0, 17, 0), (SIZE * 2, SECTOR, 0),
                                       (0, SECTOR, 2**63 - 1)):
            before = (base / "empty").stat()
            try:
                clone(a.fileno(), b.fileno(), offset, length, target)
            except OSError as e:
                assert e.errno == errno.EINVAL, e
            else:
                raise AssertionError("invalid clone succeeded")
            after = (base / "empty").stat()
            assert (after.st_size, after.st_mtime_ns, after.st_blocks) == (
                before.st_size, before.st_mtime_ns, before.st_blocks)
        clone(a.fileno(), b.fileno(), 0, 0, 0)
        try:
            clone(b.fileno(), a.fileno(), 0, 0, 0)
        except OSError as e:
            assert e.errno == errno.EBADF
        else:
            raise AssertionError("read-only destination fd accepted")
    with (base / "source").open("r+b") as a:
        try:
            clone(a.fileno(), a.fileno(), 0, 2 * SECTOR, SECTOR)
        except OSError as e:
            assert e.errno == errno.EINVAL
        else:
            raise AssertionError("overlapping clone succeeded")
        os.fsync(a.fileno())
    verify(base)
    print("range sharing, ordered data, sparse growth and validation passed")


def verify(base):
    data = payload()
    assert (base / "source").read_bytes() == data + data[:2 * SECTOR]
    assert (base / "copy").read_bytes() == data
    assert (base / "slice").read_bytes() == data[3 * SECTOR:12 * SECTOR]
    assert (base / "split").read_bytes() == (
        b"X" * (5 * SECTOR) + data[3 * SECTOR:14 * SECTOR] +
        b"X" * (SIZE - 16 * SECTOR))
    assert (base / "tailcopy").read_bytes() == b"T" * (SECTOR + 37)
    assert (base / "gap").read_bytes() == (
        b"G" * 37 + bytes(4 * SECTOR - 37) + data[:SECTOR])
    assert (base / "zero").read_bytes() == (
        data[:3 * SECTOR] + bytes(7 * SECTOR) + data[10 * SECTOR:])
    assert (base / "zero").stat().st_blocks * 512 == SIZE - 7 * SECTOR
    print("cloned contents and byte accounting verified")


def mutate(base):
    with (base / "source").open("r+b") as f:
        os.pwrite(f.fileno(), b"S" * SECTOR, 3 * SECTOR)
    assert (base / "copy").read_bytes() == payload()
    with (base / "copy").open("r+b") as f:
        os.pwrite(f.fileno(), b"C" * SECTOR, 8 * SECTOR)
        os.ftruncate(f.fileno(), 12 * SECTOR + 19)
        os.ftruncate(f.fileno(), SIZE)
        os.fsync(f.fileno())
    for name in ("source", "alias", "split", "gap", "tail"):
        (base / name).unlink()
    # The last original source is gone; the slice's checksums must survive.
    verify_mutated(base)


def verify_mutated(base):
    data = bytearray(payload()[:12 * SECTOR + 19])
    data[8 * SECTOR:9 * SECTOR] = b"C" * SECTOR
    assert (base / "copy").read_bytes() == data + bytes(SIZE - len(data))
    assert (base / "slice").read_bytes() == payload()[3 * SECTOR:12 * SECTOR]
    assert (base / "tailcopy").read_bytes() == b"T" * (SECTOR + 37)
    print("COW, truncate/regrow and source removal verified")


def race(base):
    """Reciprocal clones, writes, truncate and rename share vnode lock order."""
    base.mkdir()
    for name in ("a", "b"):
        (base / name).write_bytes(b"R" * (16 * SECTOR))

    def worker(reverse):
        src, dst = ("b", "a") if reverse else ("a", "b")
        with (base / src).open("r+b") as a, (base / dst).open("r+b") as b:
            for i in range(40):
                clone(a.fileno(), b.fileno(), 0, 4 * SECTOR, 8 * SECTOR)
                os.pwrite(a.fileno(), b"R" * SECTOR, 0)
                os.ftruncate(a.fileno(), 16 * SECTOR)
                if i % 5 == 0:
                    os.fsync(a.fileno())

    def renamer():
        for _ in range(40):
            for name in ("a", "b"):
                os.rename(base / name, base / (name + "-moved"))
                os.rename(base / (name + "-moved"), base / name)

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(worker, reverse) for reverse in (False, True)]
        for future in futures:
            future.result()
    # Rename/link contend with clone's target lock, without pathname-open races.
    with (base / "a").open("rb") as a, (base / "b").open("r+b") as b:
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(renamer)
            for _ in range(40):
                clone(a.fileno(), b.fileno(), 0, 4 * SECTOR, 8 * SECTOR)
            future.result()
        os.fsync(b.fileno())
    assert (base / "a").read_bytes() == b"R" * (16 * SECTOR)
    assert (base / "b").read_bytes() == b"R" * (16 * SECTOR)
    print("reciprocal clone, write, truncate, sync and rename races passed")


def views(source, destination):
    """OpenBSD: immutable source and a disjoint writable destination view."""
    (destination / "viewcopy").touch()
    link_range(source / "source", destination / "viewcopy")
    assert (destination / "viewcopy").read_bytes() == payload()
    with (source / "source").open("rb") as a, (
            destination / "viewcopy").open("r+b") as b:
        # A file descriptor opened before flags change still enforces flags.
        for flags in (stat.UF_IMMUTABLE, stat.UF_APPEND):
            os.chflags(destination / "viewcopy", flags)
            try:
                clone(a.fileno(), b.fileno(), 0, SECTOR, 0)
            except OSError as e:
                assert e.errno == errno.EPERM, e
            else:
                raise AssertionError("protected destination accepted")
            finally:
                os.chflags(destination / "viewcopy", 0)
    print("disjoint mount views and destination flags passed")


def extents(image, tree="fs"):
    records = {}
    names = {}
    for item in inspect(str(image), "dump-tree", "-t", tree).split("\titem "):
        key = re.search(r"key \((\d+) (\S+) (\d+)\)", item)
        if key is None:
            continue
        ino, kind, offset = int(key[1]), key[2], int(key[3])
        if kind == "INODE_REF":
            name = re.search(r"\bname: (.*)", item)
            if name:
                names[name[1]] = ino
        if kind != "EXTENT_DATA":
            continue
        disk = re.search(r"extent data disk byte (\d+) nr (\d+)", item)
        decoded = re.search(r"extent data offset (\d+) nr (\d+)", item)
        if disk and decoded and int(disk[1]):
            records.setdefault(ino, []).append(
                (offset, int(decoded[2]), int(disk[1]),
                 int(disk[2]), int(decoded[1])))
    return {name: records.get(ino, []) for name, ino in names.items()}


def identity(records, offset):
    for start, length, disk, disk_length, decoded in records:
        if start <= offset < start + length:
            return disk, disk_length, decoded + offset - start
    return None


def disk(image):
    files = extents(image)
    assert files["source"] and files["copy"]
    for offset in range(0, SIZE, SECTOR):
        assert identity(files["source"], offset) == identity(files["copy"], offset)
    for offset in range(0, 9 * SECTOR, SECTOR):
        assert identity(files["slice"], offset) == identity(
            files["source"], offset + 3 * SECTOR)
    print("independent extent records prove full and partial allocation sharing")


def seed_imported(base):
    """Linux, mounted compress=zstd. Keep a 10 GiB file in 128 MiB of data."""
    base.mkdir()
    (base / "compressed").write_bytes(payload())
    (base / "inline").write_bytes(bytes(range(37)))
    (base / "zinline").write_bytes(b"inline" * 100)
    (base / "inline-dst").write_bytes(b"D" * 37)
    (base / "inline-gap").write_bytes(b"G" * 37)
    (base / "prealloc").touch()
    fallocate(base / "prealloc", 0, 0, SIZE)
    (base / "nocow").touch()
    subprocess.check_call(["chattr", "+C", str(base / "nocow")])
    (base / "nocow").write_bytes(payload())
    (base / "nocowcopy").touch()
    subprocess.check_call(["chattr", "+C", str(base / "nocowcopy")])
    (base / "big").touch()
    unit = 128 * SIZE
    with (base / "big").open("r+b", buffering=0) as f:
        flags = struct.unpack("=Q", fcntl.ioctl(
            f.fileno(), 0x80086601, bytes(8)))[0]
        fcntl.ioctl(f.fileno(), 0x40086602, struct.pack("=Q", flags | 0x400))
        for _ in range(128):
            f.write(payload())
        os.fsync(f.fileno())
        for offset in range(unit, 10 * 1024**3, unit):
            fcntl.ioctl(f.fileno(), 0x4020940d,
                        struct.pack("=qQQQ", f.fileno(), 0, unit, offset))
        os.fsync(f.fileno())


def imported(base):
    for name in ("compressed", "inline", "zinline", "prealloc", "big"):
        (base / (name + "copy")).touch()
        link_range(base / name, base / (name + "copy"), 0,
                   (base / name).stat().st_size)
    (base / "compressed-slice").touch()
    link_range(base / "compressed", base / "compressed-slice",
               3 * SECTOR, 21 * SECTOR)
    link_range(base / "compressed", base / "inline-dst", 0, SECTOR)
    link_range(base / "compressed", base / "inline-gap", 0, SECTOR, 4 * SECTOR)
    # Replace the middle of imported preallocation, preserving both sides.
    link_range(base / "compressed", base / "prealloc", 2 * SECTOR,
               5 * SECTOR, 4 * SECTOR)
    link_range(base / "nocow", base / "nocowcopy")
    try:
        link_range(base / "nocow", base / "compressedcopy")
    except OSError as e:
        assert e.errno == errno.EINVAL, e
    else:
        raise AssertionError("incompatible checksum policy accepted")
    with (base / "bigcopy").open("r+b") as f:
        os.fsync(f.fileno())
    verify_imported(base)


def verify_imported(base):
    assert (base / "compressedcopy").read_bytes() == payload()
    assert (base / "compressed-slice").read_bytes() == payload()[
        3 * SECTOR:24 * SECTOR]
    assert (base / "inlinecopy").read_bytes() == bytes(range(37))
    assert (base / "zinlinecopy").read_bytes() == b"inline" * 100
    assert (base / "inline-dst").read_bytes() == payload()[:SECTOR]
    assert (base / "inline-gap").read_bytes() == (
        b"G" * 37 + bytes(4 * SECTOR - 37) + payload()[:SECTOR])
    assert (base / "prealloccopy").read_bytes() == bytes(SIZE)
    assert (base / "prealloccopy").stat().st_blocks == 0
    assert (base / "prealloc").read_bytes() == (
        bytes(4 * SECTOR) + payload()[2 * SECTOR:7 * SECTOR] +
        bytes(SIZE - 9 * SECTOR))
    assert (base / "nocowcopy").read_bytes() == payload()
    with (base / "bigcopy").open("rb") as f:
        assert os.fstat(f.fileno()).st_size == 10 * 1024**3
        for offset in (0, 127 * SIZE, 128 * SIZE, 9 * 1024**3,
                       10 * 1024**3 - SIZE):
            assert os.pread(f.fileno(), SIZE, offset) == payload()
        if not (base / "big").exists():
            assert os.pread(f.fileno(), SECTOR, 2 * SIZE + SECTOR) == b"B" * SECTOR
    print("compressed slices, inline, preallocation, NODATASUM and 10 GiB verified")


def disk_imported(image):
    files = extents(image)
    for name in ("compressed", "nocow", "big"):
        assert files[name]
        for start, length, *_ in files[name]:
            for offset in (start, start + length - 1):
                assert identity(files[name], offset) == identity(
                    files[name + "copy"], offset), (name, offset)
    for offset in range(0, 21 * SECTOR, SECTOR):
        assert identity(files["compressed-slice"], offset) == identity(
            files["compressed"], offset + 3 * SECTOR)
    assert not files["prealloccopy"]
    print("10 GiB and compressed mappings share allocations; preallocation is sparse")


def mutate_imported(base):
    with (base / "compressed").open("r+b") as f:
        os.pwrite(f.fileno(), b"Z" * SECTOR, 4 * SECTOR)
        os.ftruncate(f.fileno(), 13 * SECTOR + 7)
        os.fsync(f.fileno())
    (base / "compressed").unlink()
    with (base / "nocow").open("r+b") as f:
        os.pwrite(f.fileno(), b"N" * SECTOR, 0)
    (base / "nocow").unlink()
    # Drop 10 GiB of mappings: only the surviving clone owns the allocation.
    (base / "big").unlink()
    with (base / "bigcopy").open("r+b") as f:
        os.pwrite(f.fileno(), b"B" * SECTOR, 2 * SIZE + SECTOR)
        os.fsync(f.fileno())
    verify_imported(base)


def stream_seed(base):
    (base / "source").write_bytes(payload())


def stream_mutate(base):
    (base / "fresh").write_bytes(b"fresh data" * 100000 + b"tail")
    for source, target in (("source", "parentcopy"), ("fresh", "freshcopy")):
        with (base / source).open("rb") as src, (base / target).open("wb") as dst:
            fcntl.ioctl(dst.fileno(), 0x40049409, src.fileno())


def verify_stream(base):
    assert (base / "source").read_bytes() == payload()
    assert (base / "parentcopy").read_bytes() == payload()
    for name in ("fresh", "freshcopy"):
        assert (base / name).read_bytes() == b"fresh data" * 100000 + b"tail"
    print("parent and same-root CLONE stream contents verified")


def disk_stream(image, tree):
    files = extents(image, str(tree))
    for src, dst in (("source", "parentcopy"), ("fresh", "freshcopy")):
        assert files[src] and files[dst]
        for start, length, *_ in files[src]:
            for offset in (start, start + length - 1):
                assert identity(files[src], offset) == identity(
                    files[dst], offset), (src, dst, offset)
    print("received parent and same-root CLONE commands share disk allocations")


if __name__ == "__main__":
    globals()[sys.argv[1]](*(Path(arg) for arg in sys.argv[2:]))
