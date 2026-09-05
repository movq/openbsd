#!/usr/bin/env python3
"""Empty-directory removal, detached descriptors, policy, and races."""
import errno
import os
from pathlib import Path
import select
import shutil
import stat
import sys

import filehandle
import enospc
from namespace import COLLISIONS, child_checks, expect_error, wait
from unlink import snapshot, sync


def denied(error, path):
    before = snapshot(path), snapshot(path.parent)
    expect_error(error, os.rmdir, path)
    assert (snapshot(path), snapshot(path.parent)) == before


def create(base):
    base.mkdir()
    os.chmod(base, 0o755)
    name = base / "held"
    name.mkdir()
    fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY)
    parent = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
    handle = filehandle.getfh(name)
    queue = select.kqueue()
    queue.control([
        select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                      flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                      fflags=select.KQ_NOTE_DELETE),
        select.kevent(parent, filter=select.KQ_FILTER_VNODE,
                      flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                      fflags=select.KQ_NOTE_WRITE),
    ], 0)
    old = os.fstat(fd)
    os.rmdir(name)
    expect_error(errno.ENOENT, os.lstat, name)
    assert os.fstat(fd).st_nlink == 0 and os.fstat(fd).st_size == 0
    assert os.listdir(fd) == []
    os.fchmod(fd, 0o700)
    os.fsync(fd)
    assert stat.S_IMODE(os.fstat(fd).st_mode) == 0o700
    expect_error(errno.ENOENT, lambda: os.mkdir("child", dir_fd=fd))
    expect_error(errno.ENOENT, lambda: os.open(
        "child", os.O_CREAT | os.O_WRONLY, 0o600, dir_fd=fd))
    filehandle.rejected(handle)
    events = {e.ident: e.fflags for e in queue.control(None, 2, 0)}
    assert events[fd] == select.KQ_NOTE_DELETE
    assert events[parent] == select.KQ_NOTE_WRITE
    queue.close()
    name.mkdir()
    assert name.stat().st_ino != old.st_ino
    os.close(fd)
    os.close(parent)
    filehandle.rejected(handle)
    os.rmdir(name)
    name.mkdir()
    saved_cwd = os.open(".", os.O_RDONLY | os.O_DIRECTORY)
    os.chdir(name)
    try:
        os.rmdir(name)
        expect_error(errno.ENOENT, os.getcwd)
        expect_error(errno.ENOENT, os.mkdir, "child")
    finally:
        os.fchdir(saved_cwd)
        os.close(saved_cwd)
    for collision in COLLISIONS:
        (base / collision).mkdir()
    sync(base)
    os.rmdir(base / COLLISIONS[0])
    sync(base)
    assert (base / COLLISIONS[1]).is_dir()
    os.rmdir(base / COLLISIONS[1])
    (base / "nonempty").mkdir()
    (base / "nonempty/file").write_bytes(b"child\n")
    denied(errno.ENOTEMPTY, base / "nonempty")
    os.symlink("nonempty", base / "link")
    denied(errno.ENOTDIR, base / "link")
    os.unlink(base / "link")
    expect_error(errno.EINVAL, os.rmdir, str(base / "nonempty") + "/.")
    denied(errno.ENOTEMPTY, base / "nonempty/..")
    for flag in (stat.UF_IMMUTABLE, stat.UF_APPEND):
        name.mkdir()
        os.chflags(name, flag)
        denied(errno.EPERM, name)
        os.chflags(name, 0)
        os.chflags(base, flag)
        denied(errno.EPERM, name)
        os.chflags(base, 0)
        os.rmdir(name)
    sticky = base / "sticky"
    sticky.mkdir()
    os.chmod(sticky, 0o1777)
    (sticky / "root").mkdir()
    (sticky / "owned").mkdir()
    os.chown(sticky / "owned", 65534, 65534)

    def user():
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
        denied(errno.EPERM, sticky / "root")
        os.rmdir(sticky / "owned")
        denied(errno.EACCES, base / "nonempty")

    wait(child_checks(user))
    os.rmdir(sticky / "root")
    os.rmdir(sticky)
    # Both removers can begin with a cached positive lookup.
    for i in range(16):
        name.mkdir()
        assert name.is_dir()
        children = []
        for _ in range(2):
            pid = os.fork()
            if pid == 0:
                try:
                    os.rmdir(name)
                except OSError as error:
                    os._exit(1 if error.errno == errno.ENOENT else 2)
                os._exit(0)
            children.append(pid)
        results = [os.waitpid(pid, 0)[1] for pid in children]
        assert sorted(results) == [0, 256], results
    # Recursive userland removal exercises alternating file/directory cleanup.
    for i in range(8):
        directory = base / "recursive" / str(i) / "nested"
        directory.mkdir(parents=True)
        (directory / "file").write_bytes(b"recursive\n")
    shutil.rmtree(base / "recursive")
    sync(base)
    verify(base)


def verify(base):
    assert {p.name for p in base.iterdir()} == {"nonempty"}
    assert (base / "nonempty/file").read_bytes() == b"child\n"
    assert base.stat().st_size == 2 * len("nonempty")
    assert (base / "nonempty").stat().st_size == 2 * len("file")
    print("rmdir verification passed", flush=True)


def readonly(base):
    verify(base)
    denied(errno.EROFS, base / "nonempty")

def capacity(base):
    base.mkdir()
    (base / "empty").mkdir()
    sync(base)
    enospc.main(str(base / "full"))
    denied(errno.ENOSPC, base / "empty")
    sync(base)
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    print("rmdir reservation exhaustion passed", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in (
            "create", "verify", "readonly", "capacity"):
        sys.exit(f"usage: {sys.argv[0]} create|verify|readonly|capacity directory")
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
