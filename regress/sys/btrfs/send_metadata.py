#!/usr/bin/env python3
"""Metadata send fixture, ioctl differential oracle, and stream work checks."""
import ctypes
import errno
import fcntl
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


class Key(ctypes.Structure):
    _fields_ = [("objectid", ctypes.c_uint64), ("offset", ctypes.c_uint64),
                ("type", ctypes.c_uint32), ("reserved", ctypes.c_uint32)]


class Query(ctypes.Structure):
    _fields_ = [("fd", ctypes.c_int32), ("parent_fd", ctypes.c_int32),
                ("flags", ctypes.c_uint32), ("size", ctypes.c_uint32),
                ("min", Key), ("max", Key), ("buffer", ctypes.c_void_p),
                ("blocks", ctypes.c_uint64), ("shared", ctypes.c_uint64),
                ("items", ctypes.c_uint64), ("done", ctypes.c_uint32),
                ("sectorsize", ctypes.c_uint32)]


def fallocate(path, mode, offset, length):
    libc = ctypes.CDLL(None, use_errno=True)
    libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int,
                              ctypes.c_int64, ctypes.c_int64]
    with path.open("r+b") as file:
        if libc.fallocate(file.fileno(), mode, offset, length):
            raise OSError(ctypes.get_errno(), "fallocate")


def query(fd, parent=-1, capacity=65536, keys=False, low=None, high=None):
    ctl = os.open("/dev/btrfs-control", os.O_RDONLY)
    buffer = ctypes.create_string_buffer(capacity)
    args = Query(fd=fd, parent_fd=parent, flags=int(keys),
                 buffer=ctypes.addressof(buffer))
    args.min = low or Key()
    args.max = high or Key(2**64 - 1, 2**64 - 1, 255)
    result = {}
    counters = [0, 0, 0]
    try:
        while not args.done:
            args.size = capacity
            raw = bytearray(bytes(args))
            fcntl.ioctl(ctl, 0xc0000000 | ctypes.sizeof(Query) << 16 |
                        ord("B") << 8 | 10, raw, True)
            args = Query.from_buffer_copy(raw)
            for i, value in enumerate((args.blocks, args.shared, args.items)):
                counters[i] += value
            pos = 0
            while pos < args.size:
                ino, off, kind, reserved, size, reserved2 = struct.unpack_from(
                    "=QQIIII", buffer.raw, pos)
                assert not reserved and not reserved2
                key = (ino, kind, off)
                assert key not in result, key
                assert not result or next(reversed(result)) < key
                result[key] = buffer.raw[pos + 32:pos + 32 + size]
                pos += (32 + size + 7) & ~7
            assert pos == args.size
    finally:
        os.close(ctl)
    return result, counters


def trees(current, parent, writable):
    fds = [os.open(p, os.O_RDONLY | os.O_DIRECTORY)
           for p in (current, parent, writable)]
    try:
        a, ac = query(fds[0])
        b, bc = query(fds[1])
        for source, old, left, right in (
                (fds[0], fds[1], a, b), (fds[1], fds[0], b, a)):
            expected = {k: v for k, v in left.items() if right.get(k) != v}
            actual, counters = query(source, old)
            assert actual == expected
            small, _ = query(source, old, capacity=128, keys=True)
            assert small.keys() == expected.keys()
            assert counters[1] > 0, counters
            assert counters[2] < len(left) // 2, (counters, len(left))
            print(f"diff {len(actual)} items; blocks/shared/items={counters}; "
                  f"full tree {len(left)} items")
        same, counters = query(fds[0], fds[0], capacity=32)
        assert not same and counters == [0, 1, 0], counters
        # Range pagination must match filtering a complete tree exactly.
        ino = (current / "fragmented").stat().st_ino
        ranged, _ = query(fds[0], capacity=128,
                          low=Key(ino, 0, 108),
                          high=Key(ino, 2**64 - 1, 108))
        assert ranged == {k: v for k, v in a.items() if k[:2] == (ino, 108)}
        for source, old, capacity in (
                (fds[2], -1, 65536), (fds[0], fds[2], 65536),
                (fds[0], -1, 1)):
            try:
                query(source, old, capacity)
            except OSError as error:
                assert error.errno == (errno.ENOBUFS if capacity == 1
                                       else errno.EROFS), error
            else:
                raise AssertionError("invalid tree query succeeded")
    finally:
        for fd in fds:
            os.close(fd)
    print(f"tree comparison matches independent enumeration ({ac}, {bc})")


def seed(root):
    """Linux seed: sparse size dwarfs physical allocation; metadata spans nodes."""
    (root / "many").mkdir()
    for i in range(1200):
        (root / "many" / f"{i:04}").write_bytes(f"item {i}\n".encode())
    with (root / "huge").open("wb") as f:
        f.write(b"A" * 4096)
        f.seek(100 * 1024**3 - 4096)
        f.write(b"Z" * 4096)
    with (root / "fragmented").open("wb") as f:
        for i in range(300):
            f.seek(i * 8192)
            f.write(bytes([i % 251]) * 4096)
    (root / "dense").touch()
    subprocess.run(["btrfs", "property", "set", str(root / "dense"),
                    "compression", "none"], check=True)
    (root / "dense").write_bytes(bytes(range(256)) * (256 * 1024))
    (root / "compressed").write_bytes(b"compressible data\n" * 65536)
    (root / "inline").write_bytes(b"inline contents")
    (root / "inline-changed").write_bytes(b"old inline contents")
    (root / "clear").write_bytes(b"C" * 65536)
    (root / "prealloc").write_bytes(b"P" * 16384)
    fallocate(root / "prealloc", 0, 0, 1024**3)
    (root / "shorten").write_bytes(b"T" * 65536)
    (root / "rename").write_bytes(b"R" * 1048576)
    os.link(root / "dense", root / "dense-link")
    os.setxattr(root / "dense", "user.change", b"old")


def mutate(root):
    for name, offset, data in (
            ("dense", 8 * 1024**2, b"changed" * 31),
            ("huge", 50 * 1024**3, b"middle"),
            ("compressed", 65536, b"changed compressed"),
            ("fragmented", 40 * 8192, b"changed fragmented")):
        with (root / name).open("r+b") as f:
            f.seek(offset)
            f.write(data)
    (root / "inline-changed").write_bytes(b"new inline contents")
    # Absent extent records and explicit PREALLOC mappings both replace data.
    fallocate(root / "clear", 3, 4096, 32768)
    fallocate(root / "prealloc", 16, 4096, 8192)
    os.truncate(root / "shorten", 17003)
    (root / "rename").rename(root / "renamed")
    (root / "inline").rename(root / "inline-renamed")
    os.setxattr(root / "dense", "user.change", b"new")
    (root / "many/0500").unlink()
    (root / "many/added").write_bytes(b"added item")


def stream(path):
    writes = clones = commands = 0
    bypath = {}
    with path.open("rb") as f:
        assert f.read(17) == b"btrfs-stream\0" + struct.pack("<I", 1)
        while header := f.read(10):
            length, op, _ = struct.unpack("<IHI", header)
            data = f.read(length)
            attrs, pos = {}, 0
            while pos < length:
                kind, size = struct.unpack_from("<HH", data, pos)
                attrs[kind] = data[pos + 4:pos + 4 + size]
                pos += size + 4
            assert pos == length
            commands += 1
            if op == 15:
                writes += len(attrs[19])
                name = attrs[15].decode()
                bypath[name] = bypath.get(name, 0) + len(attrs[19])
            if op == 16:
                clones += struct.unpack("<Q", attrs[24])[0]
    print(f"{commands} commands, {writes} WRITE bytes, {clones} CLONE bytes")
    print(bypath)
    if "incremental" in path.name:
        assert writes < 256 * 1024, writes
        assert clones >= 1048576, clones
        assert bypath.get("huge", 0) <= 4096, bypath
        assert bypath.get("dense", bypath.get("dense-link", 0)) <= 4096


def reads(path):
    dump = subprocess.check_output(["kdump", "-f", str(path)], text=True)
    lengths = [int(n) for n in re.findall(r"RET\s+pread (\d+)", dump)]
    assert 0 < sum(lengths) < 32768, lengths
    print(f"ktrace: {len(lengths)} file reads, {sum(lengths)} bytes total")


def eof(source, destination):
    """A renamed, shortened clone needs a WRITE for its partial sector."""
    def btrfs(*args):
        subprocess.run(["btrfs", *map(str, args)], check=True)

    btrfs("subvolume", "create", source, "eof-work")
    work = source / "eof-work"
    (work / "original").write_bytes(bytes(range(256)) * 256)
    btrfs("subvolume", "snapshot", "-r", source, "eof-work", "eof-full")
    os.truncate(work / "original", 17003)
    (work / "original").rename(work / "shortened")
    btrfs("subvolume", "snapshot", "-r", source, "eof-work", "eof-next")
    with tempfile.TemporaryDirectory() as directory:
        full, incremental = (Path(directory) / n for n in ("full", "next"))
        btrfs("send", "-f", full, source, "eof-full")
        btrfs("send", "-p", "eof-full", "-f", incremental, source, "eof-next")
        # An aligned prefix must be cloned; only the final 619 bytes are read.
        data = incremental.read_bytes()
        pos, clones, writes = 17, 0, 0
        while pos < len(data):
            length, op, _ = struct.unpack_from("<IHI", data, pos)
            clones += op == 16
            writes += op == 15
            pos += 10 + length
        assert clones and writes, (clones, writes)
        btrfs("receive", "-f", full, destination, "/")
        btrfs("receive", "-f", incremental, destination, "/")
    assert (destination / "eof-next/shortened").read_bytes() == (
        work / "shortened").read_bytes()
    assert not (destination / "eof-next/original").exists()
    print("renamed clone with partial EOF replayed correctly")


def compare(a, b):
    """Compare sparse files by allocated ranges, avoiding a 100 GiB hash."""
    for source in [a, *sorted(a.rglob("*"))]:
        target = b / source.relative_to(a)
        x, y = source.stat(), target.stat()
        assert (x.st_mode, x.st_size, x.st_uid, x.st_gid, x.st_mtime_ns) == (
            y.st_mode, y.st_size, y.st_uid, y.st_gid, y.st_mtime_ns), source
        assert {k: os.getxattr(source, k) for k in os.listxattr(source)} == {
            k: os.getxattr(target, k) for k in os.listxattr(target)}, source
        if not source.is_file():
            continue
        if source.name != "huge":
            with source.open("rb") as left, target.open("rb") as right:
                while data := left.read(1024 * 1024):
                    assert data == right.read(len(data)), source
            continue
        # Both files' data ranges are checked, so spurious received data in
        # an expected hole is caught too.
        with source.open("rb") as left, target.open("rb") as right:
            for scan in (left, right):
                pos = 0
                while pos < x.st_size:
                    try:
                        start = os.lseek(scan.fileno(), pos, os.SEEK_DATA)
                    except OSError as error:
                        assert error.errno == errno.ENXIO
                        break
                    pos = os.lseek(scan.fileno(), start, os.SEEK_HOLE)
                    for off in range(start, pos, 65536):
                        length = min(65536, pos - off)
                        assert os.pread(left.fileno(), length, off) == os.pread(
                            right.fileno(), length, off), (source, off)
    assert {p.relative_to(a) for p in a.rglob("*")} == {
        p.relative_to(b) for p in b.rglob("*")}
    assert (b / "dense").stat().st_ino == (b / "dense-link").stat().st_ino
    print("matching data, holes, hardlinks, metadata, and xattrs")


if __name__ == "__main__":
    action, *args = sys.argv[1:]
    globals()[action](*(Path(arg) for arg in args))
