#!/usr/bin/env python3
"""Reject corruption in the upper half of xxHash64 super/metadata digests."""
import ctypes
import errno
import json
import os
from pathlib import Path
import re
import sys

from mirrors import inspect, SUPERS
from reclaim_chunks import chunks
from read_cluster import repair


def damage(image, journal, kind, copies):
    sb = inspect(str(image), "dump-super")
    assert re.search(r"^csum_type\s+1 ", sb, re.M)
    if kind == "super":
        size = int(re.search(r"^total_bytes\s+(\d+)", sb, re.M)[1])
        addresses = [p for p in SUPERS if p + 4096 <= size]
    else:
        assert kind == "metadata"
        logical = int(re.search(r"^chunk_root\s+(\d+)", sb, re.M)[1])
        chunk = next(c for c in chunks(image)
                     if c["logical"] <= logical < c["logical"] + c["length"])
        addresses = [p + logical - chunk["logical"]
                     for p in chunk["physical"]]
    assert len(addresses) >= 2
    if copies == "one":
        addresses = addresses[:1]
    else:
        assert copies == "all"
    fd = os.open(image, os.O_RDWR)
    try:
        saved = [(p + 4, os.pread(fd, 1, p + 4)[0]) for p in addresses]
        journal.write_text(json.dumps(saved))
        for address, value in saved:
            assert os.pwrite(fd, bytes([value ^ 0xff]), address) == 1
        os.fsync(fd)
    finally:
        os.close(fd)
    print(f"damaged upper digest bytes: {kind}, {copies}")


def reject(device, point):
    class Args(ctypes.Structure):
        _fields_ = [("fspec", ctypes.c_char_p), ("subvolid", ctypes.c_uint64),
                    ("devices", ctypes.POINTER(ctypes.c_char_p)),
                    ("ndevices", ctypes.c_uint32)]

    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
                          ctypes.c_void_p)
    for flags in (1, 0):  # MNT_RDONLY, writable
        args = Args(os.fsencode(device), 0)
        result = libc.mount(b"btrfs", os.fsencode(point), flags,
                            ctypes.byref(args))
        assert result == -1, "corrupt digest accepted"
        assert ctypes.get_errno() == errno.EINVAL, ctypes.get_errno()
    print("both mount modes rejected corrupt xxHash64 digests")


if __name__ == "__main__":
    phase = sys.argv[1]
    if phase == "damage":
        damage(Path(sys.argv[2]), Path(sys.argv[3]), *sys.argv[4:])
    elif phase == "repair":
        repair(Path(sys.argv[2]), Path(sys.argv[3]))
    elif phase == "reject":
        reject(*sys.argv[2:])
    else:
        raise AssertionError(phase)
