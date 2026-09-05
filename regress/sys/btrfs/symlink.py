#!/usr/bin/env python3
#
# Run create, unmount/check/remount, then verify on a disposable filesystem.
#
import ctypes
import errno
import os
import stat
import sys

from namespace import COLLISIONS, child_checks, expect_error, put, wait


TARGETS = {
    "relative": "file",
    "absolute": "/dev/null",
    "directory": "subdir",
    "dangling": "missing",
    "chain": "relative",
    "loop": "loop",
    "long": "x" * 1023,
    "x" * 255: "file",
    COLLISIONS[0]: "file",
    COLLISIONS[1]: "subdir",
}
WORKERS = 4
LINKS = 48


def sync_directory():
    fd = os.open(".", os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)


def create():
    os.umask(0o027)
    put("file", b"symlink payload\n")
    os.mkdir("subdir")
    for name, target in TARGETS.items():
        expect_error(errno.ENOENT, os.lstat, name)
        os.symlink(target, name)
        # Includes a hash collision inserted after committing its peer.
        sync_directory()
    put("directory/created", b"through directory link\n")
    expect_error(errno.EEXIST, os.symlink, "other", "relative")
    expect_error(errno.EEXIST, os.symlink, "other", "dangling")
    expect_error(errno.ENAMETOOLONG, os.symlink, "x" * 1024, "too-long")
    expect_error(errno.ENAMETOOLONG, os.symlink, "file", "y" * 256)
    expect_error(errno.ENOENT, os.symlink, "", "empty")
    expect_error(errno.ENOENT, os.lstat, "empty")
    expect_error(errno.ENOENT, os.lstat, "too-long")

    os.mkdir("owned", 0o777)
    os.chown("owned", 65534, 1234)
    os.chmod("owned", 0o770)
    os.mkdir("denied", 0o700)

    def permissions():
        os.setgroups([1234])
        os.setgid(1234)
        os.setuid(65534)
        os.symlink("../file", "owned/link")
        expect_error(errno.EACCES, os.symlink, "file", "denied/link")

    wait(child_checks(permissions))
    children = []
    for worker in range(WORKERS):
        os.mkdir(f"worker-{worker}")

        def populate(worker=worker):
            for number in range(LINKS):
                os.symlink(f"{worker}:{number}:" + "t" * 990,
                           f"worker-{worker}/link-{number}")
                os.symlink("file", f"shared-{worker}-{number}")

        children.append(child_checks(populate))
    for pid in children:
        wait(pid)
    sync_directory()
    verify()


def verify():
    for name, target in TARGETS.items():
        assert os.readlink(name) == target
        info = os.lstat(name)
        assert stat.S_ISLNK(info.st_mode)
        assert stat.S_IMODE(info.st_mode) == 0o750
        assert (info.st_size, info.st_nlink) == (len(target), 1)
    assert open("relative", "rb").read() == b"symlink payload\n"
    assert open("chain", "rb").read() == b"symlink payload\n"
    assert open("absolute", "rb").read() == b""
    assert open("subdir/created", "rb").read() == b"through directory link\n"
    assert os.stat("directory/..").st_ino == os.stat(".").st_ino
    expect_error(errno.ENOENT, os.stat, "dangling")
    expect_error(errno.ELOOP, os.stat, "loop")
    info = os.lstat("owned/link")
    assert (info.st_uid, info.st_gid) == (65534, 1234)
    assert os.readlink("owned/link") == "../file"
    # readlink must respect the user's buffer without adding a terminator.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.readlink.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
    libc.readlink.restype = ctypes.c_ssize_t
    buffer = ctypes.create_string_buffer(b"????")
    assert libc.readlink(b"relative", buffer, 2) == 2
    assert buffer.raw == b"fi??\0"
    for worker in range(WORKERS):
        for number in range(LINKS):
            assert os.readlink(f"worker-{worker}/link-{number}") == \
                f"{worker}:{number}:" + "t" * 990
            assert os.readlink(f"shared-{worker}-{number}") == "file"
    for directory in (".", "subdir", "owned", "denied",
                      *(f"worker-{w}" for w in range(WORKERS))):
        names = os.listdir(directory)
        assert os.stat(directory).st_size == sum(len(n) * 2 for n in names)
    print("symlink verification passed", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("create", "verify"):
        sys.exit(f"usage: {sys.argv[0]} create|verify test-directory")
    if sys.argv[1] == "create":
        os.mkdir(sys.argv[2], 0o755)
    os.chdir(sys.argv[2])
    if sys.argv[1] == "create":
        create()
    else:
        verify()
