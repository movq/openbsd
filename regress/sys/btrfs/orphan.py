#!/usr/bin/env python3
"""Inject pending cleanup in an unmounted fixture; reject writable mounts."""
import ctypes
import errno
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

from mirrors import inspect


def inject(image, treeid, objectid):
    # Append a zero-length orphan item to the last leaf without moving data.
    # Use a fresh subvol.py seed; no existing high-objectid maintenance items.
    tree = inspect(image, "dump-tree", "-t", str(treeid))
    logical = int(re.findall(r"^leaf (\d+) items", tree, re.M)[-1])
    nodesize = int(re.search(r"^nodesize\s+(\d+)",
                            inspect(image, "dump-super"), re.M)[1])
    for item in inspect(image, "dump-tree", "-t", "chunk").split("\titem "):
        key = re.match(r"\d+ key \(\S+ CHUNK_ITEM (\d+)\)", item)
        if key is None:
            continue
        start = int(key[1])
        length = int(re.search(r"\blength (\d+)", item)[1])
        if start <= logical < start + length:
            addresses = [int(p) + logical - start for p in re.findall(
                r"stripe \d+ devid \d+ offset (\d+)", item)]
            break
    else:
        raise AssertionError("missing metadata chunk")
    fd = os.open(image, os.O_RDWR)
    try:
        block = bytearray(os.pread(fd, nodesize, addresses[0]))
        assert len(block) == nodesize and block[100] == 0
        assert struct.unpack_from("<Q", block, 88)[0] == treeid
        assert all(os.pread(fd, nodesize, p) == block for p in addresses)
        count = struct.unpack_from("<I", block, 96)[0]
        key = 101 + (count - 1) * 25
        assert struct.unpack_from("<Q", block, key)[0] < (1 << 64) - 5
        data_offset = struct.unpack_from("<I", block, key + 17)[0]
        assert data_offset >= (count + 1) * 25
        struct.pack_into("<QBQII", block, 101 + count * 25,
                         (1 << 64) - 5, 48, objectid, data_offset, 0)
        struct.pack_into("<I", block, 96, count + 1)
        crc = 0xffffffff
        for byte in block[32:]:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
        struct.pack_into("<I", block, 0, crc ^ 0xffffffff)
        for address in addresses:
            assert os.pwrite(fd, block, address) == nodesize
        os.fsync(fd)
    finally:
        os.close(fd)
    print(f"injected orphan {objectid} in tree {treeid}", flush=True)


def reject(device, mountpoint):
    class Args(ctypes.Structure):
        _fields_ = [("fspec", ctypes.c_char_p), ("subvolid", ctypes.c_uint64)]

    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
                           ctypes.c_void_p)
    # Include a clean sibling view and repeat to check failed-mount teardown.
    for treeid in (5, 259, 257, 5):
        args = Args(os.fsencode(device), treeid)
        status = libc.mount(b"btrfs", os.fsencode(mountpoint), 0,
                            ctypes.byref(args))
        if status == 0:
            subprocess.run(["umount", mountpoint], check=True)
            raise AssertionError("orphan filesystem mounted writable")
        assert ctypes.get_errno() == errno.EOPNOTSUPP, ctypes.get_errno()
    subprocess.run(["mount_btrfs", "-o", "ro", device, mountpoint], check=True)
    try:
        assert (Path(mountpoint) / "left/nested/marker").read_text() == (
            "left/nested")
        assert os.statvfs(mountpoint).f_flag & os.ST_RDONLY
    finally:
        subprocess.run(["umount", mountpoint], check=True)
    print("writable mounts rejected; read-only inspection passed", flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "inject":
        inject(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    elif len(sys.argv) == 4 and sys.argv[1] == "reject":
        reject(sys.argv[2], sys.argv[3])
    else:
        sys.exit(f"usage: {sys.argv[0]} inject image treeid objectid\n"
                 f"       {sys.argv[0]} reject device mountpoint")
