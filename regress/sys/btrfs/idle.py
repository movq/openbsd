#!/usr/bin/env python3
#
# Idle commits must preserve a writable mount and permit subsequent mutation.
# Run after a writable remount, including on an image with a large extent tree.
#
import os
from pathlib import Path
import sys


def sync_idle(directory):
    fd = os.open(directory, os.O_RDONLY)
    try:
        for _ in range(3):
            os.fsync(fd)
            os.sync()
            assert not (os.fstatvfs(fd).f_flag & os.ST_RDONLY)
    finally:
        os.close(fd)


def main(directory):
    sync_idle(directory.parent)
    directory.mkdir()
    sync_idle(directory)
    for number in range(3):
        name = directory / str(number)
        with name.open("xb") as output:
            output.write(b"after idle sync\n")
            output.flush()
            os.fsync(output.fileno())
        sync_idle(directory)
        assert name.read_bytes() == b"after idle sync\n"
    print("idle sync and subsequent mutation passed", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} new-test-directory")
    main(Path(sys.argv[1]).absolute())
