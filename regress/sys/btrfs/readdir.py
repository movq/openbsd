#!/usr/bin/env python3
"""Directory cookies, short buffers, and direct seeks on OpenBSD."""
import ctypes
import errno
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys

from mirrors import inspect


def seed(base):
    directory = base / "indexed"
    directory.mkdir(parents=True)
    for i in range(320):
        (directory / (f"{i:04d}-" + "n" * (i % 251))).touch()
    os.link(directory / "0000-", directory / "alias")
    os.symlink("0000-", directory / "link")
    (directory / "subdir").mkdir()


def gaps(image):
    """Renumber an unmounted mkfs fixture, including keys and inode refs.

    Monotonic renumbering preserves B-tree ordering and parent separator
    keys. No items move or change size. Both DUP copies get new CRC32Cs.
    Use only the seed above, without subvolumes or extended references.
    """
    nodesize = int(re.search(r"^nodesize\s+(\d+)",
                            inspect(image, "dump-super"), re.M)[1])
    chunks = []
    for item in inspect(image, "dump-tree", "-t", "chunk").split("\titem "):
        key = re.search(r"key \(\S+ CHUNK_ITEM (\d+)\)", item)
        if key:
            chunks.append((int(key[1]),
                           int(re.search(r"\blength (\d+)", item)[1]),
                           [int(p) for p in re.findall(
                               r"stripe \d+ devid \d+ offset (\d+)", item)]))
    logicals = [int(p) for p in re.findall(
        r"^\titem \d+ key \((\d+) METADATA_ITEM ",
        inspect(image, "dump-tree", "-t", "extent"), re.M)]

    def renumber(block, offset):
        index = struct.unpack_from("<Q", block, offset)[0]
        assert 2 <= index < 10000, index
        struct.pack_into("<Q", block, offset, (1 << 32) + index * 17)

    def crc32c(data):
        crc = 0xffffffff
        for byte in data:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
        return crc ^ 0xffffffff

    fd = os.open(image, os.O_RDWR)
    changed = 0
    try:
        for logical in logicals:
            start, length, mirrors = next(
                c for c in chunks if c[0] <= logical < c[0] + c[1])
            addresses = [p + logical - start for p in mirrors]
            block = bytearray(os.pread(fd, nodesize, addresses[0]))
            assert len(block) == nodesize
            if struct.unpack_from("<Q", block, 88)[0] != 5:
                continue
            assert all(os.pread(fd, nodesize, p) == block for p in addresses)
            count = struct.unpack_from("<I", block, 96)[0]
            level = block[100]
            for slot in range(count):
                key = 101 + slot * (33 if level else 25)
                kind = block[key + 8]
                if kind == 96:  # DIR_INDEX, also in internal separators
                    renumber(block, key + 9)
                if level:
                    continue
                offset, size = struct.unpack_from("<II", block, key + 17)
                offset += 101
                end = offset + size
                assert kind != 13  # No INODE_EXTREF in this fixture.
                if kind == 12:  # packed INODE_REF
                    while offset < end:
                        index, namelen = struct.unpack_from("<QH", block,
                                                           offset)
                        if index:  # The root's ".." reference has index zero.
                            renumber(block, offset)
                        offset += 10 + namelen
                    assert offset == end
            struct.pack_into("<I", block, 0, crc32c(block[32:]))
            for address in addresses:
                assert os.pwrite(fd, block, address) == nodesize
            changed += 1
        os.fsync(fd)
    finally:
        os.close(fd)
    assert changed > 1  # Exercise internal separators and multiple leaves.
    print(f"renumbered directory indexes in {changed} tree blocks", flush=True)


def getdents(fd, size):
    libc = ctypes.CDLL(None, use_errno=True)
    libc.getdents.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
    libc.getdents.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(size)
    count = libc.getdents(fd, buffer, size)
    if count < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    entries = []
    offset = 0
    while offset < count:
        ino, cookie, length, kind, namelen = struct.unpack_from(
            "=QqHBB", buffer.raw, offset)
        assert length >= 32 and length % 8 == 0
        assert offset + length <= count and 24 + namelen < length
        name = buffer.raw[offset + 24:offset + 24 + namelen].decode()
        assert buffer.raw[offset + 24 + namelen] == 0
        entries.append([name, ino, cookie, kind])
        offset += length
    assert offset == count
    if entries:
        assert os.lseek(fd, 0, os.SEEK_CUR) == entries[-1][2]
    return entries


def scan(fd, size):
    entries = []
    while True:
        batch = getdents(fd, size)
        if not batch:
            return entries
        entries += batch


def check(directory):
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        entries = scan(fd, 512)
        assert [e[0] for e in entries[:2]] == [".", ".."]
        assert len({e[0] for e in entries}) == len(entries)
        assert {e[0] for e in entries[2:]} == set(os.listdir(directory))
        assert all(a[2] < b[2] for a, b in zip(entries, entries[1:]))
        assert entries[2][2] > 1 << 32
        for name, ino, cookie, kind in entries:
            info = os.lstat(directory / name)
            assert info.st_ino == ino
            assert stat.S_IFMT(info.st_mode) >> 12 == kind
        # Resume every cookie with room for exactly one maximal entry.
        for i, entry in enumerate(entries):
            os.lseek(fd, entry[2], os.SEEK_SET)
            following = getdents(fd, 280)
            assert following == entries[i + 1:i + 1 + len(following)]
            assert bool(following) == (i + 1 < len(entries))
        # A seek into an index gap must find its successor.
        for i in range(2, len(entries) - 1):
            if entries[i + 1][2] - entries[i][2] > 2:
                os.lseek(fd, entries[i][2] + 1, os.SEEK_SET)
                assert getdents(fd, 280)[0] == entries[i + 1]
        for cookie in (0, 1, 2, entries[2][2]):
            os.lseek(fd, cookie, os.SEEK_SET)
            try:
                getdents(fd, 1)
            except OSError as error:
                assert error.errno == errno.EINVAL
            else:
                raise AssertionError("short buffer falsely reported EOF")
            assert os.lseek(fd, 0, os.SEEK_CUR) == cookie
        os.lseek(fd, 0, os.SEEK_SET)
        assert getdents(fd, 32) == entries[:1]
        assert getdents(fd, 32) == entries[1:2]
        os.lseek(fd, (1 << 63) - 1, os.SEEK_SET)
        assert getdents(fd, 512) == []
        os.lseek(fd, 0, os.SEEK_SET)
        assert scan(fd, 65536) == entries
        return entries
    finally:
        os.close(fd)


def exercise(base):
    directory = base / "indexed"
    before = check(directory)
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.lseek(fd, before[-1][2], os.SEEK_SET)
        assert getdents(fd, 512) == []
        (directory / "created-after-eof").write_bytes(b"persistent")
        os.fsync(fd)
        added = getdents(fd, 512)
        assert len(added) == 1 and added[0][0] == "created-after-eof"
        assert check(directory) == before + added
        (base / "cookies.json").write_text(json.dumps(before + added))
        os.sync()
    finally:
        os.close(fd)


def verify(base):
    assert check(base / "indexed") == json.loads(
        (base / "cookies.json").read_text())


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in (
            "seed", "gaps", "exercise", "verify"):
        sys.exit(f"usage: {sys.argv[0]} seed|gaps|exercise|verify path")
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
    print(f"readdir {sys.argv[1]} passed", flush=True)
