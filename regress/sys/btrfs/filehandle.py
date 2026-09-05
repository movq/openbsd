#!/usr/bin/env python3
"""Local OpenBSD file handles, stale identities, and subvolume boundaries."""
import ctypes
import errno
import fcntl
import json
import os
from pathlib import Path
import stat
import struct
import sys

from namespace import expect_error


LIBC = ctypes.CDLL(None, use_errno=True)
LIBC.getfh.argtypes = (ctypes.c_char_p, ctypes.c_void_p)
LIBC.fhopen.argtypes = (ctypes.c_void_p, ctypes.c_int)
LIBC.fhstat.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
HANDLE_SIZE = 28  # fsid (8) and fid header/payload (4 + 16)


def result(value):
    if value == -1:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return value


def getfh(path):
    handle = ctypes.create_string_buffer(HANDLE_SIZE)
    result(LIBC.getfh(os.fsencode(path), handle))
    return handle.raw


def fhopen(handle, flags=os.O_RDONLY):
    assert len(handle) == HANDLE_SIZE
    return result(LIBC.fhopen(handle, flags))


def fhidentity(handle):
    # Only decode the stable mode/dev/ino prefix of OpenBSD's struct stat.
    # The rest of this generously sized buffer holds times and size fields.
    output = ctypes.create_string_buffer(512)
    result(LIBC.fhstat(handle, output))
    return struct.unpack_from("=IIQ", output.raw)


def identity(path):
    info = os.stat(path)
    return info.st_mode, info.st_dev, info.st_ino


def changed(handle, offset, fmt, value):
    handle = bytearray(handle)
    struct.pack_into("=" + fmt, handle, offset, value)
    return bytes(handle)


def rejected(handle, error=errno.ESTALE):
    expect_error(error, fhopen, handle)
    expect_error(error, fhidentity, handle)


def check(base, readonly=False):
    file = base / "file"
    handle = getfh(file)
    assert getfh(base / "alias") == handle
    assert getfh(base / "symlink") == handle
    for name in ("file", "alias", "directory", "fifo", "null"):
        path = base / name
        current = getfh(path)
        assert fhidentity(current) == identity(path)
        fd = fhopen(current, os.O_RDONLY | os.O_NONBLOCK)
        info = os.fstat(fd)
        assert (info.st_mode, info.st_dev, info.st_ino) == identity(path)
        if name in ("file", "alias"):
            assert os.read(fd, 100) == b"final"
        elif name == "null":
            assert os.read(fd, 100) == b""
        os.close(fd)
    for offset, fmt, value in ((8, "H", 0), (10, "H", 2)):
        rejected(changed(handle, offset, fmt, value), errno.EINVAL)
    for offset, fmt, value in (
            (12, "I", 1), (12, "I", 1234567),
            (16, "Q", 0), (16, "Q", (1 << 64) - 1),
            (24, "I", struct.unpack_from("=I", handle, 24)[0] ^ 1)):
        rejected(changed(handle, offset, fmt, value))
    expect_error(errno.ENOTDIR, fhopen, handle, os.O_RDONLY | os.O_DIRECTORY)
    expect_error(errno.EINVAL, fhopen, handle, os.O_RDWR | os.O_CREAT)
    expect_error(errno.EISDIR, fhopen, getfh(base), os.O_WRONLY)
    if readonly:
        expect_error(errno.EROFS, fhopen, handle, os.O_RDWR)
        expect_error(errno.EROFS, fhopen, handle, os.O_RDONLY | os.O_TRUNC)
    print("file identities, aliases, types, and malformed handles passed",
          flush=True)


def exercise(base, saved):
    base.mkdir()
    (base / "file").write_bytes(b"original")
    os.link(base / "file", base / "alias")
    os.symlink("file", base / "symlink")
    (base / "directory").mkdir()
    os.mkfifo(base / "fifo")
    os.mknod(base / "null", stat.S_IFCHR | 0o600, os.makedev(2, 2))
    handle = getfh(base / "file")
    fd = fhopen(handle, os.O_RDWR | os.O_TRUNC | os.O_CLOEXEC)
    assert os.fstat(fd).st_size == 0 and not os.get_inheritable(fd)
    assert os.write(fd, b"final") == 5
    os.fsync(fd)
    assert getfh(base / "file") == handle  # Transid changes, identity does not.
    # The pathname and handle must share advisory-lock state.
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    alias = os.open(base / "alias", os.O_RDONLY)
    expect_error(errno.EWOULDBLOCK, fcntl.flock, alias,
                 fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.close(alias)
    os.close(fd)
    os.chflags(base / "file", stat.UF_IMMUTABLE)
    expect_error(errno.EPERM, fhopen, handle, os.O_WRONLY)
    os.chflags(base / "file", stat.UF_APPEND)
    expect_error(errno.EPERM, fhopen, handle, os.O_WRONLY)
    fd = fhopen(handle, os.O_WRONLY | os.O_APPEND)
    os.close(fd)
    os.chflags(base / "file", 0)
    os.sync()
    check(base)
    # These calls retain the native superuser restriction.
    pid = os.fork()
    if pid == 0:
        try:
            os.setgroups([])
            os.setgid(65534)
            os.setuid(65534)
            expect_error(errno.EPERM, getfh, base / "file")
            expect_error(errno.EPERM, fhopen, handle)
            expect_error(errno.EPERM, fhidentity, handle)
        except BaseException:
            os._exit(1)
        os._exit(0)
    assert os.waitpid(pid, 0)[1] == 0
    Path(saved).write_text(json.dumps([getfh(base / name).hex() for name in
                                     ("file", "directory", "fifo", "null")]))
    print("handle mutation, locking, flags, and privilege checks passed",
          flush=True)


def stale(saved):
    for handle in json.loads(Path(saved).read_text()):
        rejected(bytes.fromhex(handle))
    print("handles from the closed filesystem are stale", flush=True)


def subvol(left, right):
    # Mount disjoint seeded subvolumes, with nested and frozen under left.
    paths = (left / "marker", left / "nested/marker", left / "frozen/marker",
             right / "marker")
    handles = [getfh(path) for path in paths]
    for path, handle in zip(paths, handles):
        assert fhidentity(handle) == identity(path)
        fd = fhopen(handle)
        assert os.read(fd, 100) == path.read_bytes()
        os.close(fd)
    assert len(set(handles)) == len(paths)
    expect_error(errno.EROFS, fhopen, handles[2], os.O_WRONLY)
    # Keep a valid tree/inode/generation, but claim the other mount's fsid.
    # Do this after populating both vnode caches to exercise ownership checks.
    rejected(handles[0][:8] + handles[3][8:])
    rejected(handles[3][:8] + handles[1][8:])
    rejected(changed(handles[0], 12, "I", 5))
    print("nested, read-only, and disjoint-tree handle scope passed", flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "exercise":
        exercise(Path(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "subvol":
        subvol(Path(sys.argv[2]), Path(sys.argv[3]))
    elif len(sys.argv) == 3 and sys.argv[1] in ("verify", "readonly"):
        check(Path(sys.argv[2]), sys.argv[1] == "readonly")
    elif len(sys.argv) == 3 and sys.argv[1] == "stale":
        stale(sys.argv[2])
    else:
        sys.exit(f"usage: {sys.argv[0]} exercise directory saved.json\n"
                 f"       {sys.argv[0]} verify|readonly directory\n"
                 f"       {sys.argv[0]} stale saved.json\n"
                 f"       {sys.argv[0]} subvol left-mount right-mount")
