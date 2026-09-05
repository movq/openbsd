#!/usr/bin/env python3
#
# Run create, unmount/check/remount, then verify on a disposable filesystem.
#
import errno
import fcntl
import os
import select
import stat
import sys

from namespace import COLLISIONS, child_checks, expect_error, put, wait


WORKERS = 4
LINKS = 32
ALIASES = ("alias", "x" * 255, *COLLISIONS, "other/alias")
PAYLOAD = b"shared inode payload\n"


def sync_directory():
    fd = os.open(".", os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)


def create(extended=False):
    put("source", PAYLOAD)
    os.mkdir("other")
    fd = os.open("source", os.O_RDWR)
    parent = os.open(".", os.O_RDONLY)
    queue = select.kqueue()
    queue.control([
        select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                      flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                      fflags=select.KQ_NOTE_LINK),
        select.kevent(parent, filter=select.KQ_FILTER_VNODE,
                      flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                      fflags=select.KQ_NOTE_WRITE),
    ], 0)
    before = os.fstat(fd)
    for name in ALIASES:
        expect_error(errno.ENOENT, os.lstat, name)
        os.link("source", name)
        sync_directory()  # Also expands a committed hash collision bucket.
    events = {event.ident: event for event in queue.control(None, 2, 0)}
    assert events[fd].fflags == select.KQ_NOTE_LINK
    assert events[parent].fflags == select.KQ_NOTE_WRITE
    queue.close()
    os.close(parent)
    after = os.fstat(fd)
    assert after.st_ctime_ns >= before.st_ctime_ns
    assert after.st_mtime_ns == before.st_mtime_ns
    assert after.st_blocks == before.st_blocks
    assert after.st_nlink == 1 + len(ALIASES)
    assert os.pwrite(fd, b"S", 0) == 1
    assert open("other/alias", "rb").read() == b"S" + PAYLOAD[1:]
    os.chmod("alias", 0o600)
    assert stat.S_IMODE(os.stat("source").st_mode) == 0o600
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def locked_alias():
        alias = os.open("other/alias", os.O_RDONLY)
        expect_error(errno.EAGAIN, fcntl.flock, alias,
                     fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.close(alias)

    wait(child_checks(locked_alias))
    fcntl.flock(fd, fcntl.LOCK_UN)
    os.fsync(fd)
    os.close(fd)
    expect_error(errno.EEXIST, os.link, "source", "alias")
    expect_error(errno.EPERM, os.link, "other", "directory-link")
    expect_error(errno.ENAMETOOLONG, os.link, "source", "x" * 256)
    expect_error(errno.EXDEV, os.link, "/etc/passwd", "cross-device")

    os.symlink("missing", "symlink")
    # OpenBSD link(2) follows symlinks; linkat without AT_SYMLINK_FOLLOW
    # creates an alias of the symlink inode itself.
    os.link("symlink", "symlink-alias", follow_symlinks=False)
    expect_error(errno.EEXIST, os.link, "source", "symlink-alias")

    os.mkdir("denied", 0o700)
    os.mkdir("owned", 0o777)
    os.chown("owned", 65534, 1234)
    os.chmod("owned", 0o700)

    def permissions():
        os.setgroups([1234])
        os.setgid(1234)
        os.setuid(65534)
        put("owned/source", b"owned\n")
        os.chmod("owned/source", 0o400)
        os.link("owned/source", "owned/alias")
        expect_error(errno.EACCES, os.link, "owned/source", "denied/alias")

    wait(child_checks(permissions))
    children = []
    for worker in range(WORKERS):
        os.mkdir(f"worker-{worker}")

        def populate(worker=worker):
            for number in range(LINKS):
                os.link("source", f"worker-{worker}/link-{number}")
                os.link("source", f"shared-{worker}-{number}")

        children.append(child_checks(populate))
    for pid in children:
        wait(pid)

    # Exercise inode-ref overflow, with and without EXTENDED_IREF support.
    os.mkdir("capacity")
    put("capacity/source", b"capacity\n")
    count = 0
    while True:
        name = f"capacity/{count:04d}-" + "n" * 240
        before_dir = os.stat("capacity")
        before_inode = os.stat("capacity/source")
        try:
            os.link("capacity/source", name)
        except OSError as error:
            assert not extended, error
            assert error.errno == errno.EMLINK, error
            expect_error(errno.ENOENT, os.lstat, name)
            after_dir = os.stat("capacity")
            after_inode = os.stat("capacity/source")
            assert (after_dir.st_size, after_dir.st_mtime_ns,
                    after_dir.st_ctime_ns) == (
                        before_dir.st_size, before_dir.st_mtime_ns,
                        before_dir.st_ctime_ns)
            assert (after_inode.st_nlink, after_inode.st_ctime_ns) == (
                before_inode.st_nlink, before_inode.st_ctime_ns)
            break
        count += 1
        if extended and count == 256:
            break
        assert count < 1024
    put("capacity-count", str(count).encode())
    if extended:
        # Exhaust the ordinary reference item's tail with short names, so
        # both equal-length colliding names must use extended references.
        for number in range(256):
            os.link("capacity/source", f"capacity/pad-{number:03d}")
        for name in COLLISIONS:
            os.link("capacity/source", f"capacity/{name}")
            sync_directory()
        put("extended", b"")
    # A different parent has room for an ordinary inode reference.
    os.link("capacity/source", "capacity-alias")
    sync_directory()
    verify()


def verify():
    source = os.stat("source")
    assert source.st_nlink == 1 + len(ALIASES) + WORKERS * LINKS * 2
    assert stat.S_IMODE(source.st_mode) == 0o600
    names = list(ALIASES)
    names += [f"worker-{w}/link-{n}"
              for w in range(WORKERS) for n in range(LINKS)]
    names += [f"shared-{w}-{n}"
              for w in range(WORKERS) for n in range(LINKS)]
    for name in names:
        info = os.stat(name)
        assert (info.st_ino, info.st_nlink, info.st_blocks) == (
            source.st_ino, source.st_nlink, source.st_blocks)
        assert open(name, "rb").read() == b"S" + PAYLOAD[1:]
    for name in ("symlink", "symlink-alias"):
        info = os.lstat(name)
        assert stat.S_ISLNK(info.st_mode) and info.st_nlink == 2
        assert info.st_ino == os.lstat("symlink").st_ino
        assert os.readlink(name) == "missing"
    assert os.stat("owned/source").st_ino == os.stat("owned/alias").st_ino
    assert os.stat("owned/alias").st_nlink == 2
    count = int(open("capacity-count").read())
    source = os.stat("capacity/source")
    extended = os.path.exists("extended")
    assert source.st_nlink == count + 2 + (258 if extended else 0)
    for number in range(count):
        assert os.stat(f"capacity/{number:04d}-" + "n" * 240).st_ino == \
            source.st_ino
    if extended:
        for name in (*COLLISIONS, *(f"pad-{n:03d}" for n in range(256))):
            assert os.stat(f"capacity/{name}").st_ino == source.st_ino
    for directory in (".", "other", "owned", "denied", "capacity",
                      *(f"worker-{w}" for w in range(WORKERS))):
        names = os.listdir(directory)
        assert os.stat(directory).st_size == sum(len(n) * 2 for n in names)
    print("hard link verification passed", flush=True)


def readonly():
    expect_error(errno.EROFS, os.link, "source", "readonly-alias")
    verify()


def limit():
    put("source", b"link limit\n")
    maximum = os.pathconf("source", "PC_LINK_MAX")
    fd = os.open("source", os.O_RDONLY)
    for number in range(maximum - 1):
        os.link("source", f"link-{number:05d}-" + "n" * 64)
        if number % 512 == 511:
            os.fsync(fd)
        if number % 4096 == 4095:
            print(f"created {number + 1} links", flush=True)
    before = os.fstat(fd)
    parent = os.stat(".")
    expect_error(errno.EMLINK, os.link, "source", "over-limit")
    expect_error(errno.ENOENT, os.lstat, "over-limit")
    assert os.fstat(fd).st_ctime_ns == before.st_ctime_ns
    assert os.stat(".").st_size == parent.st_size
    assert os.stat(".").st_mtime_ns == parent.st_mtime_ns
    sync_directory()
    os.close(fd)
    verify_limit()


def verify_limit():
    source = os.stat("source")
    maximum = os.pathconf("source", "PC_LINK_MAX")
    assert source.st_nlink == maximum
    with os.scandir(".") as entries:
        names = []
        for entry in entries:
            assert entry.inode() == source.st_ino
            names.append(entry.name)
    assert len(names) == maximum
    # Check every readdir identity and sample pathname lookups.  This also
    # bounds verification on kernels whose lookup still scans the directory.
    for name in names[::4096]:
        assert os.stat(name).st_ino == source.st_ino
    assert os.stat(".").st_size == sum(len(n) * 2 for n in names)
    assert open("source", "rb").read() == b"link limit\n"
    print(f"verified {maximum} links to one inode", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in (
            "create", "create-extended", "verify", "readonly",
            "limit", "verify-limit"):
        sys.exit(f"usage: {sys.argv[0]} "
                 "create|create-extended|verify|readonly|limit|verify-limit "
                 "test-directory")
    if sys.argv[1] in ("create", "create-extended", "limit"):
        os.mkdir(sys.argv[2], 0o755)
    os.chdir(sys.argv[2])
    if sys.argv[1] == "create-extended":
        create(extended=True)
    else:
        globals()[sys.argv[1].replace("-", "_")]()
