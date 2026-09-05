#!/usr/bin/env python3
"""chflags policy and persistence on a disposable writable btrfs mount."""
import errno
import os
from pathlib import Path
import select
import stat
import sys

from namespace import child_checks, expect_error, put, wait


def snapshot(name):
    info = os.stat(name)
    return (info.st_flags, info.st_mode, info.st_uid, info.st_gid,
            info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def denied(name, code, function, *args):
    before = snapshot(name)
    expect_error(code, function, *args)
    assert snapshot(name) == before


def enforcement():
    fd = os.open("file", os.O_RDWR)
    os.chflags("file", stat.UF_IMMUTABLE)
    assert os.stat("alias").st_flags == stat.UF_IMMUTABLE
    denied("file", errno.EPERM, os.pwrite, fd, b"X", 0)
    denied("file", errno.EPERM, os.chmod, "file", 0o600)
    denied("file", errno.EPERM, os.chown, "file", 1234, 1234)
    denied("file", errno.EPERM, os.utime, "file", None)
    denied("file", errno.EPERM, os.link, "file", "forbidden")
    expect_error(errno.EPERM, os.open, "file", os.O_WRONLY)
    os.chflags("alias", stat.UF_APPEND)
    denied("file", errno.EPERM, os.pwrite, fd, b"X", 0)
    denied("file", errno.EPERM, os.link, "file", "forbidden")
    append = os.open("file", os.O_WRONLY | os.O_APPEND)
    assert os.write(append, b"tail") == 4
    os.close(append)
    os.chflags("file", 0)
    assert os.pwrite(fd, b"D", 0) == 1
    os.fsync(fd)
    os.close(fd)
    assert Path("file").read_bytes() == b"Datatail"

    os.mkdir("directory")
    os.chflags("directory", stat.UF_IMMUTABLE)
    denied("directory", errno.EPERM, os.mkdir, "directory/blocked")
    denied("directory", errno.EPERM, os.link, "file", "directory/link")
    os.chflags("directory", stat.UF_APPEND)
    put("directory/allowed", b"child")
    os.link("file", "directory/link")
    os.chflags("directory", 0)


def permissions():
    put("owned", b"owner")
    os.chown("owned", 1234, 1234)

    def owner():
        os.setgroups([])
        os.setgid(1234)
        os.setuid(1234)
        os.chflags("owned", stat.UF_IMMUTABLE | stat.UF_NODUMP)
        assert os.stat("owned").st_flags == (
            stat.UF_IMMUTABLE | stat.UF_NODUMP)
        os.chflags("owned", stat.UF_APPEND)
        os.chflags("owned", 0)
        denied("file", errno.EPERM, os.chflags, "file", stat.UF_NODUMP)
        denied("owned", errno.EOPNOTSUPP, os.chflags, "owned",
               stat.SF_IMMUTABLE)

    wait(child_checks(owner))


def create():
    put("file", b"data")
    os.link("file", "alias")
    enforcement()
    permissions()
    fd = os.open("file", os.O_RDONLY)
    queue = select.kqueue()
    queue.control([select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                   flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                   fflags=select.KQ_NOTE_ATTRIB)], 0, 0)
    os.chflags("file", stat.UF_NODUMP)
    events = queue.control(None, 4, 5)
    assert len(events) == 1 and events[0].fflags == select.KQ_NOTE_ATTRIB
    for flag in (stat.UF_OPAQUE, stat.SF_IMMUTABLE, stat.SF_APPEND,
                 stat.SF_ARCHIVED, 0x8000):
        denied("file", errno.EOPNOTSUPP, os.chflags, "file",
               stat.UF_NODUMP | flag)
    assert queue.control(None, 4, 0) == []
    queue.close()
    os.close(fd)
    # Non-regular inodes share the same metadata interface.
    os.mkfifo("fifo")
    os.chflags("fifo", stat.UF_NODUMP)
    for name, flag in (("nodump", stat.UF_NODUMP),
                       ("immutable", stat.UF_IMMUTABLE),
                       ("append", stat.UF_APPEND)):
        put(name, name.encode())
        os.chflags(name, flag)
    os.sync()
    verify()


def verify():
    assert Path("file").read_bytes() == b"Datatail"
    assert os.stat("file").st_flags == stat.UF_NODUMP
    assert os.stat("alias").st_flags == stat.UF_NODUMP
    assert os.stat("fifo").st_flags == stat.UF_NODUMP
    assert os.stat("owned").st_flags == 0
    assert os.stat("directory").st_flags == 0
    for name, flag in (("nodump", stat.UF_NODUMP),
                       ("immutable", stat.UF_IMMUTABLE),
                       ("append", stat.UF_APPEND)):
        assert Path(name).read_bytes() == name.encode()
        assert os.stat(name).st_flags == flag
    assert not Path("forbidden").exists()
    assert not Path("directory/blocked").exists()


def readonly():
    verify()
    for name in ("file", "immutable", "append", "directory", "fifo"):
        denied(name, errno.EROFS, os.chflags, name, 0)


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase == "create":
        os.mkdir(directory, 0o755)
    os.chdir(directory)
    globals()[phase]()
    print("flags", phase, "ok", flush=True)
