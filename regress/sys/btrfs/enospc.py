#!/usr/bin/env python3
#
# Fill an existing metadata block group on a disposable writable filesystem.
# Use a small image (e.g. mkfs.btrfs -b 128M -n 4096); no chunk growth is
# currently supported.  The mount must remain usable after reservation ENOSPC.
#
import errno
import os
import sys


def snapshot(path):
    st = os.stat(path)
    return st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns, st.st_nlink


def main(path):
    os.mkdir(path)
    os.chdir(path)
    count = 0
    for group in range(1024):
        directory = f"group-{group:04d}"
        before = snapshot(".")
        try:
            os.mkdir(directory)
        except OSError as error:
            assert error.errno == errno.ENOSPC, error
            assert snapshot(".") == before
            assert not os.path.exists(directory)
            break
        for number in range(128):
            name = directory + f"/{number:04d}-" + "x" * 240
            before = snapshot(directory)
            try:
                fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            except OSError as error:
                assert error.errno == errno.ENOSPC, error
                assert snapshot(directory) == before
                assert not os.path.exists(name)
                break
            os.close(fd)
            count += 1
        else:
            if group % 8 == 7:
                print(f"created {count} files", flush=True)
                fd = os.open(".", os.O_RDONLY)
                os.fsync(fd)
                os.close(fd)
            continue
        break
    else:
        raise AssertionError("did not exhaust metadata; use a smaller image")
    assert count > 0
    assert not (os.statvfs(".").f_flag & os.ST_RDONLY)
    actual = sum(len(os.listdir(name)) for name in os.listdir("."))
    assert actual == count, (actual, count)
    os.sync()
    assert not (os.statvfs(".").f_flag & os.ST_RDONLY)
    print(f"ENOSPC passed after {count} files", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} new-test-directory")
    main(sys.argv[1])
