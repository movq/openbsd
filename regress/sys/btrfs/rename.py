#!/usr/bin/env python3
"""Atomic rename, replacement, ancestry, packed items, and vnode races."""
import errno
import os
from pathlib import Path
import select
import shutil
import stat
import sys

import filehandle
from namespace import COLLISIONS, child_checks, expect_error, wait
from unlink import snapshot, sync


def denied(code, source, target):
    paths = [source, target, source.parent, target.parent]
    before = [snapshot(p) if os.path.lexists(p) else None for p in paths]
    expect_error(code, os.rename, source, target)
    assert before == [
        snapshot(p) if os.path.lexists(p) else None for p in paths]


def create(base):
    base.mkdir()
    os.chmod(base, 0o755)
    a, b = base / "a", base / "b"
    a.mkdir()
    b.mkdir()
    source, target = a / "source", b / "target"
    source.write_bytes(b"source payload\n")
    target.write_bytes(b"replaced payload\n")
    old = snapshot(source)
    source_handle = filehandle.getfh(source)
    target_handle = filehandle.getfh(target)
    sf = os.open(source, os.O_RDONLY)
    tf = os.open(target, os.O_RDWR)
    parents = [os.open(p, os.O_RDONLY | os.O_DIRECTORY) for p in (a, b)]
    q = select.kqueue()
    events = [(sf, select.KQ_NOTE_RENAME), (tf, select.KQ_NOTE_DELETE)]
    events += [(fd, select.KQ_NOTE_WRITE) for fd in parents]
    q.control([select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                            flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                            fflags=note) for fd, note in events], 0)
    os.rename(source, target)
    assert not source.exists() and target.read_bytes() == b"source payload\n"
    assert snapshot(target)[:4] == old[:4]
    assert snapshot(target)[5] == old[5]
    assert filehandle.getfh(target) == source_handle
    filehandle.rejected(target_handle)
    assert os.fstat(tf).st_nlink == 0
    assert os.pread(tf, 64, 0) == b"replaced payload\n"
    os.pwrite(tf, b"R", 0)
    os.fsync(tf)
    assert os.pread(tf, 64, 0) == b"Replaced payload\n"
    assert {e.ident: e.fflags for e in q.control(None, 4, 0)} == dict(events)
    q.close()
    for fd in [sf, tf] + parents:
        os.close(fd)
    # Cached absent and positive names, same-directory index reuse.
    expect_error(errno.ENOENT, os.stat, b / "renamed")
    os.rename(target, b / "renamed")
    os.link(b / "renamed", b / "alias")
    before = snapshot(b / "alias"), snapshot(b)
    os.rename(b / "renamed", b / "alias")
    assert before == (snapshot(b / "alias"), snapshot(b))
    assert (b / "renamed").exists()
    source.write_bytes(b"replacement source\n")
    os.rename(source, b / "alias")
    assert (b / "renamed").stat().st_nlink == 1

    # Remove/add records in one hash bucket, including replacement.
    x, y = [a / name for name in COLLISIONS]
    x.write_bytes(b"x")
    os.rename(x, y)
    x.write_bytes(b"new x")
    sync(base)
    os.rename(x, y)
    assert y.read_bytes() == b"new x"
    os.rename(y, x)
    assert x.read_bytes() == b"new x"

    # Force both ordinary and extended references, then rename within their
    # packed parent item, across parents, and over another inode's long name.
    names = [f"{i:04d}-" + "n" * 240 for i in range(192)]
    for name in names:
        os.link(b / "renamed", a / name)
    sync(base)
    for i, name in enumerate(names):
        dest = (a if i % 2 else b) / (name[:-1] + "r")
        os.rename(a / name, dest)
    for i, name in enumerate(names):
        dest = (a if i % 2 else b) / (name[:-1] + "r")
        assert dest.stat().st_ino == (b / "renamed").stat().st_ino
        os.unlink(dest)
    assert (b / "renamed").stat().st_nlink == 1

    d = a / "directory"
    d.mkdir()
    (d / "child").mkdir()
    (d / "child/file").write_bytes(b"directory payload\n")
    (b / "empty").mkdir()
    held = os.open(b / "empty", os.O_RDONLY | os.O_DIRECTORY)
    moved = os.open(d, os.O_RDONLY | os.O_DIRECTORY)
    os.rename(d, b / "empty")
    assert os.fstat(held).st_nlink == 0 and os.listdir(held) == []
    assert os.stat("..", dir_fd=moved).st_ino == b.stat().st_ino
    expect_error(errno.ENOENT, lambda: os.mkdir("no", dir_fd=held))
    os.close(held)
    os.close(moved)
    os.rename(b / "empty", a / "moved")
    assert (a / "moved/child/file").read_bytes() == b"directory payload\n"
    denied(errno.EINVAL, a / "moved", a / "moved/child/loop")
    denied(errno.ENOTEMPTY, a / "moved/child", a / "moved")
    denied(errno.ENOTEMPTY, b, a / "moved")
    denied(errno.EISDIR, b / "renamed", a / "moved")
    denied(errno.ENOTDIR, a / "moved", b / "renamed")
    expect_error(errno.EINVAL, os.rename, str(a / "moved") + "/.", a / "dot")
    expect_error(errno.EINVAL, os.rename, a / "moved", str(b) + "/.")
    expect_error(errno.EINVAL, os.rename, a / "moved", str(b) + "/..")

    os.symlink("missing", a / "link")
    os.rename(a / "link", b / "link")
    assert os.readlink(b / "link") == "missing"
    os.mkfifo(a / "fifo")
    os.rename(a / "fifo", b / "fifo")
    os.mknod(a / "device", stat.S_IFCHR | 0o600, os.makedev(2, 2))
    os.rename(a / "device", b / "device")

    source.write_bytes(b"policy")
    target.write_bytes(b"target")
    for flag in (stat.UF_IMMUTABLE, stat.UF_APPEND):
        for flagged in (source, target, a, b):
            os.chflags(flagged, flag)
            denied(errno.EPERM, source, target)
            os.chflags(flagged, 0)
    sticky = base / "sticky"
    sticky.mkdir()
    os.chmod(sticky, 0o1777)
    (sticky / "root").write_bytes(b"root")
    (sticky / "owned").write_bytes(b"owned")
    os.chown(sticky / "owned", 65534, 65534)
    writable = base / "writable"
    writable.mkdir()
    os.chmod(writable, 0o777)
    for name in ("source", "target"):
        (writable / name).mkdir()
        os.chown(writable / name, 65534, 65534)
        os.chmod(writable / name, 0o555)

    def user():
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
        denied(errno.EPERM, sticky / "root", sticky / "new")
        denied(errno.EPERM, sticky / "owned", sticky / "root")
        os.rename(sticky / "owned", sticky / "new")
        denied(errno.EACCES, source, b / "new")
        denied(errno.EACCES, writable / "source", writable / "new")
        os.chmod(writable / "source", 0o755)
        denied(errno.EACCES, writable / "source", writable / "target")
        os.chmod(writable / "target", 0o755)
        os.rename(writable / "source", writable / "target")

    wait(child_checks(user))
    sync(base)
    verify(base)
    print("rename create passed", flush=True)


def races(base):
    base.mkdir()
    a, b = base / "a", base / "b"
    a.mkdir()
    b.mkdir()
    def rename(src, dst):
        before = snapshot(src)
        for attempt in range(8):
            try:
                os.rename(src, dst)
                return
            except OSError as error:
                if error.errno != errno.ENOSPC:
                    raise
                assert snapshot(src) == before
                # Concurrent orphan cleanup can hold commit reservations.
                # This source is private to the worker; the shared target
                # can legitimately have been replaced by another worker.
                sync(base)
        raise AssertionError("rename did not recover after reservation pressure")

    # Opposing parent locks and concurrent name lookup, link, and removal.
    def mover(worker):
        for i in range(128):
            src = (a if worker % 2 else b) / f"{worker}-{i}"
            dst = (b if worker % 2 else a) / f"{worker}-{i}"
            src.write_bytes(f"{worker}:{i}".encode())
            rename(src, dst)
            assert dst.read_bytes() == f"{worker}:{i}".encode()
            os.unlink(dst)

    children = [child_checks(lambda w=w: mover(w)) for w in range(4)]
    for pid in children:
        wait(pid)

    # The target changes between namei and VOP_RENAME. All completed moves
    # consume their source; a lookup observer must never see a missing target.
    target = a / "target"
    target.write_bytes(b"initial")
    def replacer(worker):
        for i in range(100):
            source = b / f"source-{worker}"
            source.write_bytes(f"{worker}:{i}".encode())
            rename(source, target)
            assert not source.exists()

    def observer():
        for _ in range(1500):
            assert target.read_bytes()

    children = [child_checks(lambda w=w: replacer(w)) for w in range(4)]
    children.append(child_checks(observer))
    for pid in children:
        wait(pid)

    # One of two opposite directory moves must reject the new ancestry.
    for i in range(32):
        x, y = a / f"x{i}", a / f"y{i}"
        x.mkdir()
        y.mkdir()
        def move(src, dst):
            try:
                os.rename(src, dst / "child")
            except OSError as error:
                assert error.errno in (errno.ENOENT, errno.EINVAL)
        children = [child_checks(lambda: move(x, y)),
                    child_checks(lambda: move(y, x))]
        for pid in children:
            wait(pid)
        assert x.exists() != y.exists()
    shutil.rmtree(base)
    print("rename races passed", flush=True)


def verify(base):
    assert (base / "b/renamed").read_bytes() == b"source payload\n"
    assert (base / "b/alias").read_bytes() == b"replacement source\n"
    assert (base / "a/moved/child/file").read_bytes() == b"directory payload\n"
    assert (base / "a" / COLLISIONS[0]).read_bytes() == b"new x"
    assert os.readlink(base / "b/link") == "missing"
    assert stat.S_ISFIFO((base / "b/fifo").stat().st_mode)
    assert (base / "b/device").stat().st_rdev == os.makedev(2, 2)
    assert not (base / "sticky/owned").exists()
    assert (base / "sticky/new").read_bytes() == b"owned"


def capacity(base):
    from orphan_cleanup import fill
    base.mkdir()
    source, target = base / "source", base / "target"
    source.write_bytes(b"source")
    target.write_bytes(b"target")
    (base / "directory").mkdir()
    (base / "empty").mkdir()
    reserve = os.open(base / "orphan", os.O_CREAT | os.O_RDWR, 0o600)
    fill(reserve, 512)
    os.unlink(base / "orphan")
    count = 0
    while True:
        try:
            os.symlink("missing", base / (f"{count:06d}-" + "n" * 230))
        except OSError as error:
            assert error.errno == errno.ENOSPC
            break
        count += 1
    denied(errno.ENOSPC, source, target)
    denied(errno.ENOSPC, source, base / "absent")
    denied(errno.ENOSPC, base / "directory", base / "empty")
    sync(base)
    # Last-close cleanup releases the margin needed for ordinary unlink.
    os.close(reserve)
    for name in list(base.iterdir()):
        if name.name[:1].isdigit():
            name.unlink()
    os.rename(source, target)
    os.rename(base / "directory", base / "empty")
    assert target.read_bytes() == b"source"
    sync(base)
    print(f"rename capacity passed after {count} creations", flush=True)


def packed(base):
    """Run on a fixture formatted without EXTENDED_IREF."""
    base.mkdir()
    a, b = base / "a", base / "b"
    a.mkdir()
    b.mkdir()
    source = b / "source"
    source.write_bytes(b"packed")
    names = []
    for i in range(1024):
        name = a / (f"{i:04d}-" + "n" * 250)
        try:
            os.link(source, name)
        except OSError as error:
            assert error.errno == errno.EMLINK
            break
        names.append(name)
    else:
        raise AssertionError("format without extref")
    assert names
    # Removal supplies exactly the room needed for the replacement reference.
    dest = a / ("r" * 255)
    os.rename(names[0], dest)
    assert dest.stat().st_ino == source.stat().st_ino
    denied(errno.EMLINK, source, a / ("x" * 255))
    assert source.read_bytes() == b"packed"
    sync(base)
    print(f"rename packed capacity passed with {len(names)} references")


def readonly(base):
    denied(errno.EROFS, base / "b/renamed", base / "b/alias")
    denied(errno.EROFS, base / "a/moved", base / "b/new")
    verify(base)


def subvol(base):
    """Use subvol.py's left/nested/frozen/right seed."""
    source = base / "left/source"
    source.write_bytes(b"subvolume rename")
    denied(errno.EXDEV, source, base / "right/target")
    denied(errno.EBUSY, base / "left/nested", base / "left/new")
    denied(errno.EBUSY, base / "left/frozen", base / "left/new")
    (base / "left/empty").mkdir()
    denied(errno.EBUSY, base / "left/empty", base / "left/nested")
    os.rename(source, base / "left/renamed")
    assert (base / "left/renamed").read_bytes() == b"subvolume rename"
    assert not source.exists()
    sync(base)
    print("rename subvolume boundaries passed")


def hold(base):
    import signal
    base.mkdir()
    (base / "source").write_bytes(b"source\n" * 4096)
    (base / "target").write_bytes(b"old target\n" * 4096)
    fd = os.open(base / "target", os.O_RDWR)
    (base / "old-ino").write_text(str(os.fstat(fd).st_ino))
    os.rename(base / "source", base / "target")
    sync(base)
    assert os.fstat(fd).st_nlink == 0
    Path("/tmp/btrfs-rename-ready").write_text("ready\n")
    signal.pause()


def recovered(base):
    assert not (base / "source").exists()
    assert (base / "target").read_bytes() == b"source\n" * 4096
    assert (base / "target").stat().st_ino != int(
        (base / "old-ino").read_text())
    print("rename recovery passed")


if __name__ == "__main__":
    globals()[sys.argv[1]](Path(sys.argv[2]).resolve())
