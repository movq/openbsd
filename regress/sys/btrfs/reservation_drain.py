#!/usr/bin/env python3
"""Metadata reservation pressure, same-generation reuse, and crash rollback."""
import os
from pathlib import Path
import sys

from unlink import sync

COUNT = 4096


def create(base):
    base.mkdir()
    for i in range(COUNT):
        (base / f"old-{i}").touch(mode=0o600)
    sync(base)
    verify_original(base)


def verify_original(base):
    assert sorted(p.name for p in base.iterdir()) == sorted(
        f"old-{i}" for i in range(COUNT))
    for i in range(COUNT):
        st = (base / f"old-{i}").stat()
        assert st.st_size == 0 and st.st_mode & 0o777 == 0o600


def mutate(base):
    # Each pass repeatedly revisits metadata materialized by earlier drains.
    for i in range(COUNT):
        os.rename(base / f"old-{i}", base / f"new-{i}")
    for i in range(COUNT):
        path = base / f"new-{i}"
        path.unlink()
        path.touch(mode=0o640)
    for i in range(COUNT):
        (base / f"new-{i}").chmod(0o644)
    verify(base)


def verify(base):
    assert sorted(p.name for p in base.iterdir()) == sorted(
        f"new-{i}" for i in range(COUNT))
    for i in range(COUNT):
        st = (base / f"new-{i}").stat()
        assert st.st_size == 0 and st.st_mode & 0o777 == 0o644
    print("metadata reservation pressure verified", flush=True)


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
