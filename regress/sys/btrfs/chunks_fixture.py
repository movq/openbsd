#!/usr/bin/env python3
"""Shrink a fresh image's system group to exercise protected system growth."""
import os
from pathlib import Path
import re
import struct
import sys

from mirrors import inspect, SUPERS


def checksum(block):
    crc = 0xffffffff
    for byte in block[32:]:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    struct.pack_into("<I", block, 0, crc ^ 0xffffffff)


def shrink_system(image):
    supertext = inspect(image, "dump-super")
    nodesize = int(re.search(r"^nodesize\s+(\d+)", supertext, re.M)[1])
    chunks = []
    system = None
    for text in inspect(image, "dump-tree", "-t", "chunk").split("\titem "):
        key = re.search(r"key \(\S+ CHUNK_ITEM (\d+)\)", text)
        if not key:
            continue
        logical = int(key[1])
        length = int(re.search(r"\blength (\d+)", text)[1])
        physical = [int(p) for p in re.findall(
            r"stripe \d+ devid \d+ offset (\d+)", text)]
        chunks.append((logical, length, physical))
        if "type SYSTEM|" in text:
            assert system is None
            system = chunks[-1]
    assert system
    start, oldlength, stripes = system
    length = nodesize * 96
    assert length < oldlength
    removed = (oldlength - length) * len(stripes)
    for logical in re.findall(
            r"key \((\d+) METADATA_ITEM ", inspect(image, "dump-tree", "-t", "extent")):
        logical = int(logical)
        if start <= logical < start + oldlength:
            assert logical + nodesize <= start + length
    fd = os.open(image, os.O_RDWR)
    try:
        def addresses(logical):
            begin, span, physical = next(
                c for c in chunks if c[0] <= logical < c[0] + c[1])
            return [p + logical - begin for p in physical]

        trees = ["chunk", "dev", "extent"]
        if "FREE_SPACE_TREE" in supertext:
            trees.append("free-space")
        if "BLOCK_GROUP_TREE" in supertext:
            trees.append("block-group")
        for tree in trees:
            text = inspect(image, "dump-tree", "-t", tree)
            leaves = re.findall(r"^leaf (\d+) items", text, re.M)
            assert len(leaves) == 1 and not re.search(r"^node ", text, re.M)
            copies = addresses(int(leaves[0]))
            block = bytearray(os.pread(fd, nodesize, copies[0]))
            assert all(os.pread(fd, nodesize, p) == block for p in copies)
            count = struct.unpack_from("<I", block, 96)[0]
            records = []
            for i in range(count):
                ino, kind, offset, pos, size = struct.unpack_from(
                    "<QBQII", block, 101 + i * 25)
                data = bytearray(block[101 + pos:101 + pos + size])
                if tree == "chunk" and kind == 228 and offset == start:
                    struct.pack_into("<Q", data, 0, length)
                elif tree == "chunk" and kind == 216:
                    used = struct.unpack_from("<Q", data, 16)[0]
                    struct.pack_into("<Q", data, 16, used - removed)
                elif tree == "dev" and kind == 204 and offset in stripes:
                    struct.pack_into("<Q", data, 24, length)
                elif kind == 192 and ino == start:
                    assert offset == oldlength
                    offset = length
                elif tree == "free-space":
                    if kind == 198 and ino == start:
                        offset = length
                    elif start <= ino < start + oldlength and kind == 199:
                        assert ino < start + length
                        offset = min(offset, start + length - ino)
                    assert kind != 200, "use a fresh extent-format fixture"
                records.append((ino, kind, offset, data))
            block[101:] = bytes(nodesize - 101)
            pos = nodesize - 101
            for i, (ino, kind, offset, data) in enumerate(records):
                pos -= len(data)
                assert pos >= 25 * len(records)
                struct.pack_into("<QBQII", block, 101 + i * 25,
                                 ino, kind, offset, pos, len(data))
                block[101 + pos:101 + pos + len(data)] = data
            checksum(block)
            for address in copies:
                assert os.pwrite(fd, block, address) == nodesize
        for physical in SUPERS:
            block = bytearray(os.pread(fd, 4096, physical))
            if block[64:72] != b"_BHRfS_M":
                continue
            device_size = struct.unpack_from("<Q", block, 209)[0]
            if physical + 4096 > device_size:
                continue
            used = struct.unpack_from("<Q", block, 217)[0]
            struct.pack_into("<Q", block, 217, used - removed)
            size = struct.unpack_from("<I", block, 160)[0]
            pos = 811
            while pos < 811 + size:
                ino, kind, logical = struct.unpack_from("<QBQ", block, pos)
                assert kind == 228
                if logical == start:
                    struct.pack_into("<Q", block, pos + 17, length)
                mirrors = struct.unpack_from("<H", block, pos + 17 + 44)[0]
                pos += 17 + 48 + mirrors * 32
            assert pos == 811 + size
            checksum(block)
            assert os.pwrite(fd, block, physical) == 4096
        os.fsync(fd)
    finally:
        os.close(fd)
    print(f"system fixture shrunk from {oldlength} to {length}", flush=True)


if __name__ == "__main__":
    shrink_system(Path(sys.argv[1]))
