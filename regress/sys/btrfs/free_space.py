#!/usr/bin/env python3
"""Unmounted bitmap fixtures and cross-bitmap extent retirement."""
import ctypes
import errno
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

from mirrors import inspect
from chunks_fixture import checksum


def seed(directory):
    directory.mkdir()
    (directory / "a-prefix").write_bytes(b"P" * 4096)
    with (directory / "imported").open("xb") as output:
        for _ in range(12 * 256):
            output.write(b"B" * 4096)


def rewrite(image, mode):
    """Rewrite only a fresh fixture's single free-space leaf, in every copy."""
    sb = inspect(image, "dump-super")
    nodesize = int(re.search(r"^nodesize\s+(\d+)", sb, re.M)[1])
    sector = int(re.search(r"^sectorsize\s+(\d+)", sb, re.M)[1])
    csum_type = int(re.search(r"^csum_type\s+(\d+)", sb, re.M)[1])
    tree = inspect(image, "dump-tree", "-t", "free-space")
    leaves = re.findall(r"^leaf (\d+) items", tree, re.M)
    assert len(leaves) == 1 and not re.search(r"^node ", tree, re.M)
    logical = int(leaves[0])
    chunks = []
    addresses = []
    for item in inspect(image, "dump-tree", "-t", "chunk").split("\titem "):
        key = re.search(r"key \(\S+ CHUNK_ITEM (\d+)\)", item)
        if key:
            start = int(key[1])
            length = int(re.search(r"\blength (\d+)", item)[1])
            chunks.append((start, length))
            if start <= logical < start + length:
                addresses = [int(p) + logical - start for p in re.findall(
                    r"stripe \d+ devid \d+ offset (\d+)", item)]
    assert addresses
    if mode == "bitmap":
        crossed = False
        for start, length in re.findall(
                r"key \((\d+) EXTENT_ITEM (\d+)\)",
                inspect(image, "dump-tree", "-t", "extent")):
            start, length = int(start), int(length)
            group, _ = next(c for c in chunks if c[0] <= start < c[0] + c[1])
            boundary = group + 65536
            crossed |= start < boundary < start + length
        assert crossed, "seed must contain an allocation crossing a bitmap boundary"
    fd = os.open(image, os.O_RDWR)
    try:
        block = bytearray(os.pread(fd, nodesize, addresses[0]))
        assert len(block) == nodesize and block[100] == 0
        assert all(os.pread(fd, nodesize, p) == block for p in addresses)
        count = struct.unpack_from("<I", block, 96)[0]
        records = []
        for i in range(count):
            start, kind, length, offset, size = struct.unpack_from(
                "<QBQII", block, 101 + i * 25)
            records.append((start, kind, length,
                            bytes(block[101 + offset:101 + offset + size])))
        if mode in ("bitmap", "bitmap-groups"):
            converted = []
            for i, (start, kind, length, payload) in enumerate(records):
                if kind != 198:
                    assert kind == 199
                    continue
                expected, flags = struct.unpack("<II", payload)
                assert flags == 0
                bits = bytearray((length // sector + 7) // 8)
                for free_start, free_kind, free_length, _ in records[i + 1:]:
                    if free_kind == 198:
                        break
                    assert free_kind == 199
                    for bit in range((free_start - start) // sector,
                                     (free_start + free_length - start) // sector):
                        bits[bit // 8] |= 1 << (bit % 8)
                converted.append((start, 198, length,
                                  struct.pack("<II", expected, 1)))
                offset = 0
                while offset < length:
                    # A short first bitmap puts a boundary inside imported
                    # data mappings even when mkfs makes 8 MiB data groups.
                    span = min(length - offset,
                               65536 if offset == 0 else sector * 2048)
                    begin = offset // sector // 8
                    data = bytes(bits[begin:begin + (span // sector + 7) // 8])
                    converted.append((start + offset, 200, span, data))
                    offset += span
            records = converted
        elif mode == "bad-count":
            start, kind, length, payload = records[0]
            assert kind == 198
            count, flags = struct.unpack("<II", payload)
            records[0] = (start, kind, length, struct.pack("<II", count + 1, flags))
        elif mode == "bad-bit":
            # Preserve the run count but make one free sector unavailable.
            previous = 0
            for i, (start, kind, length, payload) in enumerate(records):
                if kind == 198:
                    previous = 0
                    continue
                assert kind == 200
                changed = bytearray(payload)
                for bit in range(length // sector - 1):
                    current = (payload[bit // 8] >> (bit % 8)) & 1
                    following = (payload[(bit + 1) // 8] >> ((bit + 1) % 8)) & 1
                    if not previous and current and following:
                        changed[bit // 8] &= ~(1 << (bit % 8))
                        records[i] = (start, kind, length, bytes(changed))
                        break
                    previous = current
                else:
                    previous = (payload[-1] >> ((length // sector - 1) % 8)) & 1
                    continue
                if changed != payload:
                    break
            else:
                raise AssertionError("no free run")
        else:
            raise AssertionError(mode)
        block[101:] = bytes(nodesize - 101)
        offset = nodesize - 101
        for i, (start, kind, length, payload) in enumerate(records):
            offset -= len(payload)
            assert offset >= len(records) * 25
            struct.pack_into("<QBQII", block, 101 + i * 25,
                             start, kind, length, offset, len(payload))
            block[101 + offset:101 + offset + len(payload)] = payload
        struct.pack_into("<I", block, 96, len(records))
        checksum(block, csum_type)
        for address in addresses:
            assert os.pwrite(fd, block, address) == nodesize
        os.fsync(fd)
    finally:
        os.close(fd)
    print(f"free-space fixture: {mode}", flush=True)


def exercise(base):
    name = base / "imported"
    assert name.read_bytes() == b"B" * (12 * 1024 * 1024)
    fd = os.open(name, os.O_RDWR)
    try:
        # Retain pieces of mappings that cross bitmap boundaries,
        # then drop its last reference and retire the entire allocation.
        os.ftruncate(fd, 8 * 1024 * 1024 + 4096)
        os.fsync(fd)
        assert os.pread(fd, 4096, 8 * 1024 * 1024) == b"B" * 4096
        os.ftruncate(fd, 0)
        os.fsync(fd)
        os.write(fd, b"bitmap reuse\n")
        os.fsync(fd)
    finally:
        os.close(fd)
    assert name.read_bytes() == b"bitmap reuse\n"
    print("cross-bitmap extent retirement passed", flush=True)


def reject(device, mountpoint):
    class Args(ctypes.Structure):
        _fields_ = [("fspec", ctypes.c_char_p), ("subvolid", ctypes.c_uint64)]

    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
                          ctypes.c_void_p)
    for _ in range(2):
        args = Args(os.fsencode(device), 0)
        result = libc.mount(b"btrfs", os.fsencode(mountpoint), 0,
                            ctypes.byref(args))
        if result == 0:
            subprocess.run(["umount", mountpoint], check=True)
            raise AssertionError("inconsistent free space mounted writable")
        assert ctypes.get_errno() == errno.EINVAL, ctypes.get_errno()
    subprocess.run(["mount_btrfs", "-o", "ro", device, mountpoint], check=True)
    try:
        assert (Path(mountpoint) / "imported").read_bytes() == b"B" * (12 * 1024 * 1024)
    finally:
        subprocess.run(["umount", mountpoint], check=True)
    print("inconsistent free space rejected; read-only data intact", flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] in ("seed", "exercise"):
        globals()[sys.argv[1]](Path(sys.argv[2]))
    elif len(sys.argv) == 3 and sys.argv[1] in (
            "bitmap", "bitmap-groups", "bad-count", "bad-bit"):
        rewrite(sys.argv[2], sys.argv[1])
    elif len(sys.argv) == 4 and sys.argv[1] == "reject":
        reject(*sys.argv[2:])
    else:
        sys.exit(f"usage: {sys.argv[0]} seed|exercise directory\n"
                 f"       {sys.argv[0]} bitmap|bitmap-groups|bad-count|bad-bit image\n"
                 f"       {sys.argv[0]} reject device mountpoint")
