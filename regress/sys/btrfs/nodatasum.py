#!/usr/bin/env python3
"""COW writes with and without data checksums on imported regular files."""
import errno
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

from inline import original
from namespace import expect_error


POLICIES = ("checksummed", "nodatasum", "nodatacow")
SIZES = (0, 100, 6003, 16384)
FINAL = 32775


def seed(base):
    for policy in POLICIES:
        directory = base / policy
        directory.mkdir(parents=True)
        for size in SIZES:
            for operation in ("write", "grow"):
                (directory / f"{operation}-{size}").write_bytes(original(size))
        (directory / "capacity").write_bytes(original(100))


def format_image(base, image, nodesize, holes):
    command = ["mkfs.btrfs", "-f", "-b", "1G", "-n", nodesize, "-s", "4096",
               "-m", "dup", "-d", "single", "-O",
               "^free-space-tree,^block-group-tree" +
               (",^no-holes" if holes == "holes" else ""),
               "--rootdir", str(base)]
    for policy in POLICIES[1:]:
        for path in sorted((base / policy).iterdir()):
            command += ["--inode-flags", f"{policy}:{path.relative_to(base)}"]
    subprocess.run(command + [image], check=True)


def expected(operation, size):
    data = bytearray(original(size) + bytes(FINAL - size))
    if operation == "write":
        data[7:18] = b"replacement"
        data[4093:4101] = b"boundary"
        data[:4096] = b"R" * 4096
    data[-4:] = b"tail"
    return bytes(data)


def exercise(base):
    # Alternate policies in one open transaction to share checksum-tree leaves.
    for size in SIZES:
        for operation in ("write", "grow"):
            for policy in POLICIES:
                path = base / policy / f"{operation}-{size}"
                fd = os.open(path, os.O_RDWR)
                assert os.read(fd, size + 1) == original(size)
                if operation == "write":
                    # A distant write first converts any inline prefix.
                    assert os.pwrite(fd, b"tail", FINAL - 4) == 4
                    assert os.pwrite(fd, b"replacement", 7) == 11
                    assert os.pwrite(fd, b"boundary", 4093) == 8
                    # Replace an already ordered sector repeatedly.
                    for byte in (b"Q", b"R"):
                        assert os.pwrite(fd, byte * 4096, 0) == 4096
                else:
                    os.ftruncate(fd, size + 1)
                    os.ftruncate(fd, FINAL)
                    assert os.pwrite(fd, b"tail", FINAL - 4) == 4
                assert os.pread(fd, FINAL + 1, 0) == expected(operation, size)
                os.close(fd)
    os.sync()
    # Replace committed sectors, including all pieces of an imported extent.
    for policy in POLICIES:
        path = base / policy / "write-16384"
        fd = os.open(path, os.O_RDWR)
        content = expected("write", 16384)
        for offset in range(0, 16384, 4096):
            assert os.pwrite(fd, content[offset:offset + 4096], offset) == 4096
        os.fsync(fd)
        os.close(fd)
        os.link(path, base / policy / "alias")
        os.chflags(path, stat.UF_NODUMP)
    verify(base)


def verify(base, restored=False):
    for policy in POLICIES:
        for size in SIZES:
            for operation in ("write", "grow"):
                assert (base / policy / f"{operation}-{size}").read_bytes() == (
                    expected(operation, size))
        assert (base / policy / "capacity").read_bytes() == original(100)
        assert (base / policy / "alias").read_bytes() == expected("write", 16384)
        if not restored:
            info = (base / policy / "write-16384").stat()
            assert info.st_ino == (base / policy / "alias").stat().st_ino
            assert info.st_flags == stat.UF_NODUMP


def capacity(base):
    fd = os.open(base / "filler", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    for sector in range(32768):
        try:
            os.pwrite(fd, b"F" * 4096, sector * 4096)
        except OSError as error:
            assert error.errno == errno.ENOSPC
            break
    else:
        raise AssertionError("use a small existing data block group")
    os.fsync(fd)
    os.close(fd)
    for policy in POLICIES:
        path = base / policy / "capacity"
        before = path.stat()
        fd = os.open(path, os.O_RDWR)
        expect_error(errno.ENOSPC, os.pwrite, fd, b"fail", FINAL)
        expect_error(errno.ENOSPC, os.ftruncate, fd, FINAL)
        os.close(fd)
        after = path.stat()
        assert (before.st_size, before.st_blocks, before.st_mtime_ns,
                before.st_ctime_ns) == (after.st_size, after.st_blocks,
                                       after.st_mtime_ns, after.st_ctime_ns)
        assert path.read_bytes() == original(100)
    os.sync()
    assert not os.statvfs(base).f_flag & os.ST_RDONLY


def disk(image):
    def tree(name):
        return subprocess.check_output(
            ["btrfs", "inspect-internal", "dump-tree", "-t", name, image],
            text=True)

    ranges = [(int(start), int(end)) for start, end in re.findall(
        r"range start (\d+) end (\d+)", tree("csum"))]
    inodes = {}
    checked = [0, 0]
    for item in tree("fs").split("\titem "):
        inode = re.match(r"\d+ key \((\d+) INODE_ITEM 0\)", item)
        if inode:
            flags = int(re.search(r"flags 0x([0-9a-f]+)", item)[1], 16)
            inodes[int(inode[1])] = flags
            if flags & 2:  # NODATACOW implies NODATASUM.
                assert flags & 1
        extent = re.match(r"\d+ key \((\d+) EXTENT_DATA \d+\)", item)
        mapping = re.search(r"extent data disk byte (\d+) nr (\d+)", item)
        if extent and mapping:
            start, length = map(int, mapping.groups())
            if start == 0:
                continue
            nodatasum = bool(inodes[int(extent[1])] & 1)
            for sector in range(start, start + length, 4096):
                present = any(a <= sector < b for a, b in ranges)
                assert present != nodatasum, (extent[1], sector, nodatasum)
            checked[nodatasum] += 1
    assert all(checked), checked
    print(f"checked checksum policy for {checked} regular mappings")


if __name__ == "__main__":
    phase, path, *options = sys.argv[1:]
    base = Path(path).resolve()
    if phase == "format":
        format_image(base, *options)
    elif phase == "verify-restored":
        verify(base, restored=True)
    else:
        globals()[phase](base)
    print("nodatasum", phase, "passed", flush=True)
