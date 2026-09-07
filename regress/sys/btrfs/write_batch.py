#!/usr/bin/env python3
"""Large writes must use a smaller reservation when only a sector fits."""
import errno
import os
from pathlib import Path
import sys

SECTOR = 4096
WINDOW = 65536


def capacity(base):
    base.mkdir()
    (base / "spare").write_bytes(b"S" * SECTOR)
    target = os.open(base / "target", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    filler = os.open(base / "filler", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        # Large writes can leave a successful prefix on ENOSPC. Finish filling
        # sector by sector so the fixture also works with unbatched writers.
        for size in (WINDOW, SECTOR):
            for _ in range(65536):
                end = os.fstat(filler).st_size
                try:
                    assert os.pwrite(filler, b"F" * size, end) == size
                except OSError as error:
                    assert error.errno == errno.ENOSPC, error
                    break
            else:
                raise AssertionError("use a small filesystem")
        os.fsync(filler)
        os.unlink(base / "spare")
        os.sync()
        try:
            os.write(target, b"T" * WINDOW)
        except OSError as error:
            assert error.errno == errno.ENOSPC, error
        else:
            raise AssertionError("more than one sector remained available")
        assert os.fstat(target).st_size == SECTOR
        assert os.pread(target, WINDOW, 0) == b"T" * SECTOR
        os.fsync(target)
        assert not os.statvfs(base).f_flag & os.ST_RDONLY
    finally:
        os.close(filler)
        os.close(target)
    verify(base)


def verify(base):
    assert not (base / "spare").exists()
    assert (base / "target").read_bytes() == b"T" * SECTOR
    with (base / "filler").open("rb") as stream:
        while data := stream.read(WINDOW):
            assert data == b"F" * len(data)
    print("large-write reservation fallback and committed prefix verified",
          flush=True)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
