#!/usr/bin/env python3
#
# Namespace regression for a disposable, already-mounted btrfs filesystem.
# Run create, unmount/check/remount, then verify.  Requires root and Python 3.
#
import errno
import os
import stat
import sys


COLLISIONS = (
    "collision-3646043d825f0094b1055539",
    "collision-71a40ea0311399a610d2bf0e",
)  # Both have btrfs name hash 0x93c5d6bd.
WORKERS = 4
FILES = 160
LONG_NAME = "x" * 255


def expect_error(code, function, *args):
    try:
        function(*args)
    except OSError as error:
        assert error.errno == code, (function, error, code)
    else:
        raise AssertionError((function, "unexpected success"))


def put(name, payload, sync=False):
    fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o640)
    try:
        assert os.write(fd, payload) == len(payload)
        assert os.pread(fd, len(payload), 0) == payload
        if sync:
            os.fsync(fd)
    finally:
        os.close(fd)


def child_checks(function):
    pid = os.fork()
    if pid == 0:
        try:
            function()
        except BaseException:
            import traceback
            traceback.print_exc()
            os._exit(1)
        os._exit(0)
    return pid


def wait(pid):
    assert os.waitpid(pid, 0)[1] == 0, ("child failed", pid)


def payload(worker, number):
    return (f"{worker}:{number}\n".encode() * 31)


def filename(number):
    return f"file-{number:04d}-" + "n" * 180


def create():
    os.umask(0o027)
    os.mkdir("nested", 0o777)
    os.mkdir("nested/child", 0o755)
    # Populate a negative name-cache entry before creation.
    expect_error(errno.ENOENT, os.stat, "negative")
    put("negative", b"negative cache invalidated\n")
    put(LONG_NAME, b"maximum name\n")
    for name in COLLISIONS:
        put(name, name.encode(), sync=True)
    expect_error(errno.EEXIST, os.mkdir, "nested")
    expect_error(errno.EEXIST, put, COLLISIONS[0], b"duplicate")
    expect_error(errno.ENAMETOOLONG, os.mkdir, "z" * 256)
    expect_error(errno.ENAMETOOLONG, put, "z" * 256, b"")
    expect_error(errno.ENOTDIR, os.mkdir, "negative/child")
    expect_error(errno.EOPNOTSUPP, os.mkfifo, "unsupported-fifo")
    put("sparse", b"start")
    fd = os.open("sparse", os.O_RDWR)
    assert os.pwrite(fd, b"end", 8191) == 3
    assert os.pread(fd, 8194, 0) == b"start" + bytes(8186) + b"end"
    os.fsync(fd)
    os.close(fd)
    os.chmod("negative", 0o600)

    # OpenBSD inherits the parent's group for both files and directories.
    os.mkdir("permissions", 0o777)
    os.chown("permissions", 65534, 1234)
    os.chmod("permissions", 0o2770)
    os.mkdir("denied", 0o700)

    def permissions():
        os.setgroups([1234])
        os.setgid(1234)
        os.setuid(65534)
        put("permissions/owned", b"unprivileged\n")
        os.mkdir("permissions/owned-dir", 0o750)
        expect_error(errno.EACCES, put, "denied/no-file", b"")
        expect_error(errno.EACCES, os.mkdir, "denied/no-dir")

    wait(child_checks(permissions))
    children = []
    for worker in range(WORKERS):
        os.mkdir(f"worker-{worker}", 0o750)

        def populate(worker=worker):
            for number in range(FILES):
                put(f"worker-{worker}/{filename(number)}",
                    payload(worker, number), number % 64 == 63)
            # Contend on one parent as well as independent parents.
            for number in range(32):
                put(f"shared-{worker}-{number}", payload(worker, number))

        children.append(child_checks(populate))
    for pid in children:
        wait(pid)
    fd = os.open(".", os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)
    verify()


def verify():
    names = set(os.listdir("."))
    expected = {"nested", "negative", LONG_NAME, *COLLISIONS, "sparse",
                "permissions", "denied"}
    expected.update(f"worker-{worker}" for worker in range(WORKERS))
    expected.update(f"shared-{worker}-{number}"
                    for worker in range(WORKERS) for number in range(32))
    assert names == expected, (names - expected, expected - names)
    root = os.stat(".")
    assert root.st_nlink == 1
    assert root.st_size == sum(len(name) * 2 for name in names)
    assert os.stat("nested/child/..").st_ino == os.stat("nested").st_ino
    assert os.stat("nested/..").st_ino == root.st_ino
    assert stat.S_IMODE(os.stat("nested").st_mode) == 0o750
    assert stat.S_IMODE(os.stat("negative").st_mode) == 0o600
    assert open("negative", "rb").read() == b"negative cache invalidated\n"
    assert open(LONG_NAME, "rb").read() == b"maximum name\n"
    assert open("sparse", "rb").read() == b"start" + bytes(8186) + b"end"
    for name in COLLISIONS:
        assert open(name, "rb").read() == name.encode()
    for name in ("permissions/owned", "permissions/owned-dir"):
        info = os.stat(name)
        assert (info.st_uid, info.st_gid, info.st_nlink) == (65534, 1234, 1)
    inodes = set()
    for worker in range(WORKERS):
        directory = f"worker-{worker}"
        assert set(os.listdir(directory)) == {
            filename(number) for number in range(FILES)}
        for number in range(FILES):
            name = f"{directory}/{filename(number)}"
            info = os.stat(name)
            assert info.st_ino not in inodes
            inodes.add(info.st_ino)
            assert info.st_nlink == 1
            assert stat.S_IMODE(info.st_mode) == 0o640
            assert open(name, "rb").read() == payload(worker, number)
        for number in range(32):
            assert open(f"shared-{worker}-{number}", "rb").read() == \
                payload(worker, number)
    print("namespace verification passed", flush=True)


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
