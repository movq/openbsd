#!/usr/bin/env python3
"""Unmounted device fixture for hosts without permission to create devices."""
import os
from pathlib import Path
import re
import stat
import struct
import sys

from chunks_fixture import checksum
from devices import DEVICES
from mirrors import inspect
from reclaim_chunks import chunks

NODES = dict(DEVICES, **{"overflow-major": (stat.S_IFCHR, 256, 2)})


def device_seed(directory):
    directory.mkdir()
    # mkfs imports names and references. Replace the empty inode types and
    # device numbers below, without requiring privileged host mknod.
    for name in NODES:
        (directory / name).touch()


def devices(image):
    text = inspect(str(image), "dump-tree", "-t", "fs")
    inodes = {}
    for item in text.split("\titem "):
        key = re.search(r"key \((\d+) INODE_REF 256\)", item)
        name = re.search(r"\bname: ([^\n]+)", item)
        if key and name and name[1] in NODES:
            inodes[int(key[1])] = NODES[name[1]]
    assert len(inodes) == len(NODES)
    nodesize = int(re.search(r"^nodesize\s+(\d+)",
                            inspect(str(image), "dump-super"), re.M)[1])
    maps = chunks(image)
    fd = os.open(image, os.O_RDWR)
    try:
        for logical in map(int, re.findall(r"^leaf (\d+) items", text, re.M)):
            chunk = next(c for c in maps if
                         c["logical"] <= logical < c["logical"] + c["length"])
            copies = [p + logical - chunk["logical"] for p in chunk["physical"]]
            block = bytearray(os.pread(fd, nodesize, copies[0]))
            assert all(os.pread(fd, nodesize, p) == block for p in copies)
            count = struct.unpack_from("<I", block, 96)[0]
            for i in range(count):
                ino, kind, _, offset, size = struct.unpack_from(
                    "<QBQII", block, 101 + 25 * i)
                start = 101 + offset
                if kind == 1 and ino in inodes:
                    mode, major, minor = inodes[ino]
                    assert size == 160
                    assert struct.unpack_from("<Q", block, start + 16)[0] == 0
                    struct.pack_into("<I", block, start + 52, mode | 0o600)
                    disk = (minor & 255) | (major << 8) | ((minor & ~255) << 12)
                    struct.pack_into("<Q", block, start + 56, disk)
                elif kind in (84, 96):
                    cursor = start
                    while cursor < start + size:
                        data_size, name_size = struct.unpack_from("<HH", block, cursor + 25)
                        name = bytes(block[cursor + 30:cursor + 30 + name_size]).decode()
                        if name in NODES:
                            block[cursor + 29] = 3 if NODES[name][0] == stat.S_IFCHR else 4
                        cursor += 30 + name_size + data_size
                    assert cursor == start + size
            checksum(block)
            for physical in copies:
                assert os.pwrite(fd, block, physical) == nodesize
        os.fsync(fd)
    finally:
        os.close(fd)
    print("device inode types, directory types and rdev fields installed")


if __name__ == "__main__":
    operation, path = sys.argv[1:]
    {"device-seed": device_seed, "devices": devices}[operation](Path(path))
