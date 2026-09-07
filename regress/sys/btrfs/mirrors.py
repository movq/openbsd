#!/usr/bin/env python3
#
# Host-side supplement to btrfs check.  Use an unmounted SINGLE/DUP image.
# btrfs check may succeed by retrying a damaged DUP metadata copy.
#
import os
import re
import struct
import subprocess
import sys


SUPERS = (0x10000, 0x4000000, 0x4000000000)
STRIPE = 65536


def inspect(image, *args):
    return subprocess.check_output(
        ["btrfs", "inspect-internal", *args, image], text=True)


def check(image):
    superblock = inspect(image, "dump-super")
    nodesize = int(re.search(r"^nodesize\s+(\d+)", superblock, re.M)[1])
    chunks = []
    for item in inspect(image, "dump-tree", "-t", "chunk").split("\titem "):
        # Internal-node separator keys have no chunk payload.
        key = re.search(r"^\d+ key \(\S+ CHUNK_ITEM (\d+)\)", item)
        if key is None:
            continue
        logical = int(key[1])
        length = int(re.search(r"\blength (\d+)", item)[1])
        mirrors = [int(p) for p in re.findall(
            r"stripe \d+ devid \d+ offset (\d+)", item)]
        assert mirrors and len(mirrors) <= 2
        assert re.search(r"\btype \S+\|(single|DUP)\b", item), item
        chunks.append((logical, length, mirrors))

    extents = re.findall(
        r"^\titem \d+ key \((\d+) (METADATA_ITEM|EXTENT_ITEM) (\d+)\)",
        inspect(image, "dump-tree", "-t", "extent"), re.M)
    checked = 0
    fd = os.open(image, os.O_RDONLY)
    try:
        for logical, kind, offset in extents:
            logical = int(logical)
            length = nodesize if kind == "METADATA_ITEM" else int(offset)
            assert logical >= SUPERS[0]
            chunk = next(c for c in chunks if c[0] <= logical < c[0] + c[1])
            start, chunk_length, mirrors = chunk
            assert logical + length <= start + chunk_length
            copies = []
            for physical in mirrors:
                physical += logical - start
                for super_offset in SUPERS:
                    assert (physical + length <= super_offset or
                            physical >= super_offset + STRIPE), (
                                "allocation overlaps a superblock stripe",
                                logical, physical, length, super_offset)
                if kind == "METADATA_ITEM":
                    data = os.pread(fd, nodesize, physical)
                    assert len(data) == nodesize
                    assert struct.unpack_from("<Q", data, 48)[0] == logical
                    copies.append(data)
            if copies:
                assert all(data == copies[0] for data in copies), (
                    "metadata mirrors differ", logical)
                checked += 1
    finally:
        os.close(fd)
    assert checked > 0
    print(f"verified all copies of {checked} metadata blocks "
          f"and superblock exclusion for {len(extents)} extents")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} unmounted-image")
    check(sys.argv[1])
