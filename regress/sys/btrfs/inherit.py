#!/usr/bin/env python3
"""Creation under imported NODATACOW directories and persistent inheritance."""
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import sys

from namespace import child_checks, wait
from nodatasum import disk as check_checksums
from send_receive import xattr


DATA = b"prefix" + bytes(8190) + b"tail"


def seed(base):
    for name in ("normal", "nocow", "xattr"):
        (base / name).mkdir(parents=True)
    os.setxattr(base / "xattr", "user.test", b"retain")


def make_file(path):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    assert os.write(fd, b"prefix") == 6
    os.ftruncate(fd, len(DATA))
    assert os.pwrite(fd, b"tail", len(DATA) - 4) == 4
    os.fsync(fd)
    assert os.pread(fd, len(DATA) + 1, 0) == DATA
    os.close(fd)


def exercise(base):
    for name in ("normal", "nocow", "xattr"):
        directory = base / name
        os.chflags(directory, stat.UF_NODUMP)
        (directory / "nested/deeper").mkdir(parents=True)
        make_file(directory / "file")
        make_file(directory / "nested/deeper/file")
        os.symlink("file", directory / "symlink")
        os.mkfifo(directory / "fifo")
        os.mknod(directory / "null", stat.S_IFCHR | 0o600, os.makedev(2, 2))
        with socket.socket(socket.AF_UNIX) as sock:
            sock.bind(str(directory / "socket"))

        def create_child(number):
            child = directory / f"child-{number}"
            child.mkdir()
            make_file(child / "file")

        children = [child_checks(lambda n=n: create_child(n)) for n in range(4)]
        for pid in children:
            wait(pid)
    # Linking an existing inode never changes its allocation policy.
    os.link(base / "normal/file", base / "nocow/normal-link")
    os.link(base / "nocow/file", base / "normal/nocow-link")
    os.sync()
    verify(base)


def more(base):
    for name in ("normal", "nocow", "xattr"):
        directory = base / name / "nested/deeper/after-remount"
        directory.mkdir()
        make_file(directory / "file")
    os.sync()
    verify(base)


def verify(base):
    for name in ("normal", "nocow", "xattr"):
        directory = base / name
        assert directory.stat().st_flags == stat.UF_NODUMP
        assert xattr(base, directory, "") == (
            {"user.test": b"retain".hex()} if name == "xattr" else {})
        for path in directory.rglob("*"):
            info = path.lstat()
            # Nodump is a user flag, not a creation default.
            assert info.st_flags == 0, path
            assert xattr(base, path, "") == {}, path
            if stat.S_ISREG(info.st_mode):
                assert path.read_bytes() == DATA, path
        assert (directory / "symlink").read_bytes() == DATA
        fd = os.open(directory / "null", os.O_WRONLY)
        assert os.write(fd, b"discard") == 7
        os.close(fd)
    assert (base / "normal/file").stat().st_ino == (
        base / "nocow/normal-link").stat().st_ino
    assert (base / "nocow/file").stat().st_ino == (
        base / "normal/nocow-link").stat().st_ino


def disk(image):
    tree = subprocess.check_output(
        ["btrfs", "inspect-internal", "dump-tree", "-t", "fs", image], text=True)
    inodes, entries = {}, {}
    for item in tree.split("\titem "):
        inode = re.match(r"\d+ key \((\d+) INODE_ITEM 0\)", item)
        if inode:
            mode = int(re.search(r"mode ([0-7]+)", item)[1], 8)
            flags = int(re.search(r"flags 0x([0-9a-f]+)", item)[1], 16)
            inodes[int(inode[1])] = mode, flags
        entry = re.match(r"\d+ key \((\d+) DIR_INDEX \d+\)", item)
        if entry:
            target = int(re.search(r"location key \((\d+) INODE_ITEM", item)[1])
            name = re.search(r"\bname: ([^\n]+)", item)[1]
            entries.setdefault(int(entry[1]), []).append((name, target))
    pending = [(Path(), 256)]
    checked = 0
    while pending:
        path, ino = pending.pop()
        mode, flags = inodes[ino]
        nocow = bool(path.parts and path.parts[0] in ("nocow", "xattr"))
        if path == Path("nocow/normal-link"):
            nocow = False
        elif path == Path("normal/nocow-link"):
            nocow = True
        assert bool(flags & 2) == nocow, (path, flags)
        assert bool(flags & 1) == (nocow and stat.S_ISREG(mode)), (path, flags)
        checked += 1
        pending += [(path / name, target) for name, target in entries.get(ino, [])]
    assert checked > 20
    check_checksums(image)
    print(f"checked inherited flags for {checked} paths")


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    globals()[phase](Path(directory).resolve())
    print("inherit", phase, "passed", flush=True)
