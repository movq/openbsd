#!/usr/bin/env python3
"""Clustered commit I/O and reuse of data with cached physical sectors."""
import os
from pathlib import Path
import resource
import sys

from checksums import payload, SECTOR


COUNT = 512


def create(base):
    base.mkdir()
    for cycle in range(4):
        name = base / str(cycle)
        fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        try:
            before = resource.getrusage(resource.RUSAGE_SELF).ru_oublock
            for sector in range(COUNT):
                assert os.write(fd, payload(cycle, sector)) == SECTOR
            os.fsync(fd)
            writes = resource.getrusage(resource.RUSAGE_SELF).ru_oublock - before
            # Includes metadata and superblock writes, including DUP copies.
            assert 0 < writes < COUNT, ("sector-sized submission", writes)
            print(f"cycle {cycle}: write/fsync used {writes} output operations",
                  flush=True)
            # Shrinking invalidates vnode buffers. Populate physical read
            # buffers, free the allocation, then overwrite it next cycle.
            os.ftruncate(fd, (COUNT - 1) * SECTOR)
            os.fsync(fd)
            for sector in range(COUNT - 1):
                assert os.pread(fd, SECTOR, sector * SECTOR) == \
                    payload(cycle, sector)
        finally:
            os.close(fd)
        if cycle != 3:
            os.unlink(name)
    verify(base)


def verify(base):
    assert sorted(p.name for p in base.iterdir()) == ["3"]
    with (base / "3").open("rb") as stream:
        for sector in range(COUNT - 1):
            assert stream.read(SECTOR) == payload(3, sector)
        assert stream.read() == b""
    print("clustered write and cache reuse verified", flush=True)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]))
