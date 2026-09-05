#!/usr/bin/env python3
"""Explicit hole splitting and sparse extension; import seed with mkfs."""
import os
from pathlib import Path
import sys


SECTOR = 4096
INITIAL = b"A" * SECTOR + bytes(7 * SECTOR) + b"Z" * SECTOR
CASES = {
    "left": [(SECTOR, b"L")],
    "middle": [(4 * SECTOR + 3, b"M")],
    "right": [(8 * SECTOR - 1, b"R")],
    "split": [(4 * SECTOR, b"middle"), (2 * SECTOR, b"left"),
              (6 * SECTOR, b"right")],
    "cross": [(SECTOR - 1, b"C" * (3 * SECTOR + 2))],
    "extend": [(12 * SECTOR + 7, b"extension")],
}


def seed():
    for name in CASES:
        fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        os.write(fd, b"A" * SECTOR)
        os.pwrite(fd, b"Z" * SECTOR, 8 * SECTOR)
        os.close(fd)


def expected(writes):
    data = bytearray(INITIAL)
    for offset, payload in writes:
        if offset + len(payload) > len(data):
            data.extend(bytes(offset + len(payload) - len(data)))
        data[offset:offset + len(payload)] = payload
    return bytes(data)


def write_case(name, writes):
    fd = os.open(name, os.O_RDWR)
    done = []
    for offset, payload in writes:
        assert os.pwrite(fd, payload, offset) == len(payload)
        done.append((offset, payload))
        assert Path(name).read_bytes() == expected(done), name
        os.fsync(fd)
        assert Path(name).read_bytes() == expected(done), name
    os.close(fd)


def write():
    for name, writes in CASES.items():
        write_case(name, writes)
    verify()


def extend():
    write_case("extend", CASES["extend"])


def verify():
    for name, writes in CASES.items():
        assert Path(name).read_bytes() == expected(writes), name


def capacity():
    import errno
    from enospc import main as fill_metadata
    from grow import fail
    from namespace import put

    put("capacity-empty", b"")
    put("capacity-file", b"prefix")
    put("reservoir", b"")
    fill_metadata("full")
    os.chdir("..")
    # Namespace creation reserves more than a one-hole growth operation.
    # Consume the remainder until growth itself reaches its reservation limit.
    for attempt in range(10000):
        try:
            os.truncate("reservoir", (attempt + 1) * SECTOR)
        except OSError as error:
            assert error.errno == errno.ENOSPC
            break
    else:
        raise AssertionError("growth did not reach metadata reservation limit")
    fail("capacity-empty", errno.ENOSPC, os.truncate, "capacity-empty",
         3 * SECTOR)
    fail("capacity-file", errno.ENOSPC, os.truncate, "capacity-file",
         3 * SECTOR)
    fd = os.open("capacity-file", os.O_RDWR)
    fail("capacity-file", errno.ENOSPC, os.pwrite, fd, b"fail", 3 * SECTOR)
    os.close(fd)
    assert Path("capacity-empty").read_bytes() == b""
    assert Path("capacity-file").read_bytes() == b"prefix"
    os.sync()
    assert not os.statvfs(".").f_flag & os.ST_RDONLY


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase == "seed":
        os.makedirs(directory)
    os.chdir(directory)
    globals()[phase]()
    print("holes", phase, "ok", flush=True)
