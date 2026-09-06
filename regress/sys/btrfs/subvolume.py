#!/usr/bin/env python3
"""Targeted native subvolume/snapshot tests; see README for mount/check phases."""
import errno
import fcntl
import os
from pathlib import Path
import re
import subprocess
import struct
import sys
import threading


def command(mount, operation, *paths, fail=False, readonly=False):
    args = ["btrfs", "subvolume", operation]
    if readonly:
        args.append("-r")
    args.extend([str(mount), *paths])
    result = subprocess.run(args, text=True, capture_output=True)
    if fail:
        assert result.returncode != 0, args
    else:
        assert result.returncode == 0, (args, result.stderr)
    return result.stdout


def listing(mount):
    result = {}
    for line in command(mount, "list").splitlines():
        match = re.fullmatch(r"ID (\d+) top level (\d+) (ro|rw) path (.+)", line)
        assert match, line
        result[match[4]] = (int(match[1]), int(match[2]), match[3])
    return result


def original(i):
    return bytes([i % 251]) * (8192 + i % 11)


def create(mount):
    (mount / "container").mkdir()
    command(mount, "create", "container/home")
    source = mount / "container/home"
    (source / "dir").mkdir()
    for i in range(100):
        (source / "dir" / str(i)).write_bytes(original(i))
    os.link(source / "dir/7", source / "alias")
    os.symlink("dir/7", source / "link")
    command(mount, "snapshot", "container/home", "before")
    command(mount, "snapshot", "/container/home", "frozen", readonly=True)
    assert listing(mount)["frozen"][2] == "ro"
    for i in range(100):
        assert (mount / "before/dir" / str(i)).read_bytes() == original(i)
        (source / "dir" / str(i)).write_bytes(b"source" * 900)
        if i % 3:
            (mount / "before/dir" / str(i)).write_bytes(b"snapshot" * 600)
        else:
            (mount / "before/dir" / str(i)).unlink()
    assert (mount / "frozen/alias").read_bytes() == original(7)
    assert (mount / "frozen/link").read_bytes() == original(7)
    try:
        (mount / "frozen/new").touch()
    except OSError as error:
        assert error.errno == errno.EROFS, error
    else:
        raise AssertionError("read-only snapshot accepted a write")
    command(mount, "create", "before", fail=True)
    command(mount, "create", "missing/new", fail=True)
    command(mount, "create", "../escape", fail=True)
    command(mount, "snapshot", "container", "bad-source", fail=True)
    command(mount, "delete", "container", fail=True)
    command(mount, "delete", "/", fail=True)
    command(mount, "create", "container/home/child")
    command(mount, "delete", "container/home", fail=True)
    command(mount, "delete", "container/home/child")
    with open(source / "dir/10", "rb"):
        command(mount, "delete", "container/home", fail=True)
    command(mount, "snapshot", "container/home", "latest")
    command(mount, "delete", "container/home")
    assert not source.exists()
    # Rename ordinary ancestor directories and check reconstructed root paths.
    command(mount, "create", "container/nested")
    (mount / "container").rename(mount / "renamed")
    assert "renamed/nested" in listing(mount)
    assert "container/nested" not in listing(mount)
    command(mount, "delete", "renamed/nested")
    verify(mount)


def verify(mount):
    entries = listing(mount)
    assert "container/home" not in entries
    assert {"before", "frozen", "latest"} <= entries.keys()
    for i in range(100):
        assert (mount / "frozen/dir" / str(i)).read_bytes() == original(i)
        assert (mount / "latest/dir" / str(i)).read_bytes() == b"source" * 900
        target = mount / "before/dir" / str(i)
        if i % 3:
            assert target.read_bytes() == b"snapshot" * 600
        else:
            assert not target.exists()
    assert os.stat(mount / "frozen/alias").st_ino == os.stat(
        mount / "frozen/dir/7").st_ino


def child(mount):
    """Run with only the 'latest' subvolume mounted here."""
    command(mount, "snapshot", "latest", "latest@monday", readonly=True)
    command(mount, "create", "renamed/sibling")
    assert "renamed/sibling" in listing(mount)
    command(mount, "delete", "latest", fail=True)
    command(mount, "delete", "before")
    for i in range(100):
        assert (mount / "dir" / str(i)).read_bytes() == b"source" * 900
        (mount / "dir" / str(i)).write_bytes(b"after" * 500)
    command(mount, "delete", "renamed/sibling")


def final(mount):
    for i in range(100):
        assert (mount / "latest/dir" / str(i)).read_bytes() == b"after" * 500
        assert (mount / "latest@monday/dir" / str(i)).read_bytes() == b"source" * 900
        assert (mount / "frozen/dir" / str(i)).read_bytes() == original(i)
    for path in ("latest", "latest@monday", "frozen"):
        command(mount, "delete", path)
    assert not listing(mount)
    command(mount, "create", "reused")
    (mount / "reused/probe").write_bytes(b"new allocation after final drops")


def seed(directory):
    (directory / "imported").mkdir(parents=True)
    for i in range(100):
        (directory / "imported" / str(i)).write_bytes(original(i) * 8)


def compressed(mount):
    command(mount, "snapshot", "imported", "saved", readonly=True)
    for i in range(100):
        target = mount / "imported" / str(i)
        assert target.read_bytes() == original(i) * 8
        with target.open("r+b") as file:
            file.seek(4096)
            file.write(b"changed" * 300)
            file.truncate(12345)
    command(mount, "delete", "imported")
    compressed_verify(mount)


def compressed_verify(mount):
    for i in range(100):
        assert (mount / "saved" / str(i)).read_bytes() == original(i) * 8


def boundaries(mount):
    command(mount, "create", "outer")
    command(mount, "create", "outer/nested")
    (mount / "outer/nested/contents").write_text("not recursively snapshotted")
    command(mount, "snapshot", "outer", "outer-copy")
    command(mount, "delete", "outer/nested")
    for _ in range(3):
        stub = mount / "outer-copy/nested"
        assert stub.is_dir() and list(stub.iterdir()) == []
        assert stub.stat().st_ino == 2
        assert (stub / "..").stat().st_ino == (mount / "outer-copy").stat().st_ino
        try:
            (stub / "new").touch()
        except OSError:
            pass
        else:
            raise AssertionError("created file inside snapshot boundary")
    command(mount, "create", "outer-copy/nested/new", fail=True)
    command(mount, "delete", "outer-copy/nested", fail=True)
    command(mount, "delete", "outer")
    command(mount, "delete", "outer-copy")


def race(mount):
    command(mount, "create", "racing")
    target = mount / "racing/data"
    target.write_bytes(b"A" * 4096)
    stopped = threading.Event()
    failures = []

    def writer():
        try:
            with target.open("r+b", buffering=0) as file:
                i = 0
                while not stopped.is_set():
                    os.pwrite(file.fileno(), bytes([65 + i % 20]) * 4096, 0)
                    os.fsync(file.fileno())
                    temporary = mount / "racing/temporary"
                    temporary.write_bytes(b"namespace")
                    temporary.unlink()
                    i += 1
        except BaseException as error:
            failures.append(error)

    thread = threading.Thread(target=writer)
    thread.start()
    try:
        for i in range(12):
            name = "race-snapshot-" + str(i)
            command(mount, "snapshot", "racing", name)
            data = (mount / name / "data").read_bytes()
            assert len(data) == 4096 and data == data[:1] * 4096
            command(mount, "delete", name)
    finally:
        stopped.set()
        thread.join()
    assert not failures, failures
    command(mount, "delete", "racing")


def orphans(mount):
    command(mount, "create", "orphan-source")
    target = mount / "orphan-source/unlinked"
    target.write_bytes(b"unlinked data" * 4096)
    with target.open("r+b") as file:
        target.unlink()
        command(mount, "snapshot", "orphan-source", "orphan-copy", readonly=True)
        file.write(b"source only")
    assert list((mount / "orphan-copy").iterdir()) == []
    command(mount, "delete", "orphan-source")


def orphan_verify(mount):
    assert list((mount / "orphan-copy").iterdir()) == []
    command(mount, "delete", "orphan-copy")


def capacity(mount):
    command(mount, "create", "capacity")
    target = mount / "capacity"
    for i in range(500):
        (target / str(i)).write_bytes(bytes([i % 251]) * 4096)
    os.sync()
    before = listing(mount)
    result = subprocess.run(
        ["btrfs", "subvolume", "delete", str(mount), "capacity"],
        text=True, capture_output=True, env={**os.environ, "LC_ALL": "C"})
    assert result.returncode != 0 and "No space left" in result.stderr, result
    assert listing(mount) == before
    for i in range(500):
        assert (target / str(i)).read_bytes() == bytes([i % 251]) * 4096
    (target / "after").write_bytes(b"still writable")
    assert (target / "after").read_bytes() == b"still writable"
    for file in target.iterdir():
        file.unlink()
    command(mount, "delete", "capacity")
    assert "capacity" not in listing(mount)


def abi(mount):
    layout = "=iIQQQ1024s1024s"
    list_ioctl = 0xc0000000 | (struct.calcsize(layout) << 16) | (ord("B") << 8) | 1
    create_ioctl = 0x80000000 | (struct.calcsize(layout) << 16) | (ord("B") << 8) | 2
    mountfd = os.open(mount, os.O_RDONLY)
    control = os.open("/dev/btrfs-control", os.O_RDWR)

    def reject(expected, request=list_ioctl, descriptor=control, **fields):
        values = dict(fd=mountfd, flags=0, cursor=0, id=0, parent=0,
                      source=b"", path=b"")
        values.update(fields)
        args = bytearray(struct.pack(layout, *values.values()))
        try:
            fcntl.ioctl(descriptor, request, args)
        except OSError as error:
            assert error.errno == expected, (fields, error)
        else:
            raise AssertionError(("accepted invalid ioctl", fields))

    try:
        reject(errno.EBADF, fd=-1)
        reject(errno.EINVAL, flags=0x80000000)
        reject(errno.EINVAL, id=1)
        reject(errno.EINVAL, parent=1)
        reject(errno.ENAMETOOLONG, path=b"x" * 1024)
        reject(errno.ENAMETOOLONG, source=b"x" * 1024)
        reject(errno.ENOTTY, request=0x2000427f)
        ro = os.open("/dev/btrfs-control", os.O_RDONLY)
        try:
            reject(errno.EBADF, request=create_ioctl, descriptor=ro, path=b"bad")
        finally:
            os.close(ro)
        pid = os.fork()
        if pid == 0:
            try:
                os.setuid(65534)
                reject(errno.EPERM)
                os._exit(0)
            except BaseException:
                os._exit(1)
        _, status = os.waitpid(pid, 0)
        assert status == 0, status
    finally:
        os.close(control)
        os.close(mountfd)


if __name__ == "__main__":
    phase, path = sys.argv[1:]
    globals()[phase](Path(path))
    print("subvolume:", phase, "passed")
