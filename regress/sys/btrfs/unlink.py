#!/usr/bin/env python3
"""Remove nonfinal links, preserving inode identity and packed neighbors."""
import errno
import mmap
import os
from pathlib import Path
import select
import stat
import sys

from namespace import COLLISIONS, child_checks, expect_error, wait
import enospc


def snapshot(path):
    info = os.lstat(path)
    return (info.st_ino, info.st_nlink, info.st_size, info.st_blocks,
            info.st_ctime_ns, info.st_mtime_ns)


def sync(base):
    fd = os.open(base, os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)


def denied(error, path):
    before = snapshot(path), snapshot(path.parent)
    expect_error(error, os.unlink, path)
    assert (snapshot(path), snapshot(path.parent)) == before


def create(base):
    base.mkdir()
    os.chmod(base, 0o755)
    (base / "source").write_bytes(b"retained inode\n")
    os.link(base / "source", base / "keep")
    fd = os.open(base / "source", os.O_RDWR)
    parent = os.open(base, os.O_RDONLY)
    queue = select.kqueue()
    queue.control([
        select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                      flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                      fflags=select.KQ_NOTE_DELETE),
        select.kevent(parent, filter=select.KQ_FILTER_VNODE,
                      flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                      fflags=select.KQ_NOTE_WRITE),
    ], 0)
    before = snapshot(base / "source")
    os.unlink(base / "source")
    expect_error(errno.ENOENT, os.lstat, base / "source")
    after = snapshot(base / "keep")
    assert after[:4] == (before[0], 1, before[2], before[3])
    assert after[4] >= before[4] and after[5] == before[5]
    events = {e.ident: e.fflags for e in queue.control(None, 2, 0)}
    assert events[fd] == select.KQ_NOTE_DELETE
    assert events[parent] == select.KQ_NOTE_WRITE
    queue.close()
    os.close(parent)
    os.pwrite(fd, b"R", 0)
    os.fsync(fd)
    os.close(fd)
    (base / "final").write_bytes(b"final link\n")
    os.unlink(base / "final")
    expect_error(errno.ENOENT, os.lstat, base / "final")

    # Enough names to put references into EXTENDED_IREF at both node sizes.
    names = [f"{i:04d}-" + "n" * 240 for i in range(256)]
    names += [f"pad-{i:03d}" for i in range(256)]
    names += list(COLLISIONS)
    for name in names:
        os.link(base / "keep", base / name)
    sync(base)
    # Remove colliding packed records separately, including a committed
    # bucket whose surviving neighbor must remain reachable.
    for name in COLLISIONS:
        os.unlink(base / name)
        sync(base)
        names.remove(name)
    for half in (names[::2], names[1::2]):
        for name in half:
            os.unlink(base / name)
            expect_error(errno.ENOENT, os.lstat, base / name)
        sync(base)
    assert os.stat(base / "keep").st_nlink == 1
    # Recreate a formerly cached/deleted name, then delete its new alias.
    (base / "source").write_bytes(b"new inode\n")
    os.link(base / "source", base / "alias")
    assert os.stat(base / "source").st_ino != before[0]
    os.unlink(base / "alias")

    children = []
    for worker in range(4):
        directory = base / f"worker-{worker}"
        directory.mkdir()
        for i in range(64):
            os.link(base / "keep", directory / str(i))

        def remove(directory=directory):
            for i in range(64):
                os.unlink(directory / str(i))

        children.append(child_checks(remove))
    for pid in children:
        wait(pid)
    assert os.stat(base / "keep").st_nlink == 1

    os.symlink("missing", base / "symlink")
    os.link(base / "symlink", base / "symlink-alias", follow_symlinks=False)
    os.unlink(base / "symlink-alias")
    os.mkfifo(base / "fifo")
    os.link(base / "fifo", base / "fifo-alias")
    os.unlink(base / "fifo-alias")
    os.mknod(base / "device", stat.S_IFCHR | 0o600, os.makedev(2, 2))
    os.link(base / "device", base / "device-alias")
    os.unlink(base / "device-alias")
    # Native policy is enforced before the final-link support check.
    for flag in (stat.UF_IMMUTABLE, stat.UF_APPEND):
        os.link(base / "keep", base / "flagged")
        os.chflags(base / "keep", flag)
        denied(errno.EPERM, base / "flagged")
        os.chflags(base / "keep", 0)
        os.chflags(base, flag)
        denied(errno.EPERM, base / "flagged")
        os.chflags(base, 0)
        os.unlink(base / "flagged")

    sticky = base / "sticky"
    sticky.mkdir()
    os.chmod(sticky, 0o1777)
    (sticky / "root").write_bytes(b"root\n")
    os.link(sticky / "root", sticky / "root-alias")
    (sticky / "owned").write_bytes(b"owned\n")
    os.chown(sticky / "owned", 65534, 65534)
    os.link(sticky / "owned", sticky / "owned-alias")

    def user():
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
        denied(errno.EPERM, sticky / "root-alias")
        os.unlink(sticky / "owned-alias")
        denied(errno.EACCES, base / "keep")

    wait(child_checks(user))
    os.unlink(sticky / "root-alias")
    denied(errno.EPERM, base / "worker-0")
    expect_error(errno.ENOTEMPTY, os.rmdir, sticky)
    expect_error(errno.ENOENT, os.unlink, base / "absent")
    sync(base)
    verify(base)


def verify(base):
    assert (base / "keep").read_bytes() == b"Retained inode\n"
    assert (base / "source").read_bytes() == b"new inode\n"
    assert os.readlink(base / "symlink") == "missing"
    assert stat.S_ISFIFO(os.stat(base / "fifo").st_mode)
    assert stat.S_ISCHR(os.stat(base / "device").st_mode)
    for name in ("keep", "source", "symlink", "fifo", "device",
                 "sticky/root", "sticky/owned"):
        assert os.lstat(base / name).st_nlink == 1
    for worker in range(4):
        assert list((base / f"worker-{worker}").iterdir()) == []
    assert {p.name for p in base.iterdir()} == {
        "keep", "source", "symlink", "fifo", "device", "sticky",
        *(f"worker-{i}" for i in range(4))}
    for directory in (base, base / "sticky",
                      *(base / f"worker-{i}" for i in range(4))):
        assert directory.stat().st_size == sum(
            len(os.fsencode(p.name)) * 2 for p in directory.iterdir())
    print("nonfinal unlink verification passed", flush=True)


def readonly(base):
    verify(base)
    denied(errno.EROFS, base / "keep")

def capacity(base):
    base.mkdir()
    (base / "keep").write_bytes(b"capacity\n")
    os.link(base / "keep", base / "alias")
    for name in COLLISIONS:
        os.link(base / "keep", base / name)
    names = [f"{i:04d}-" + "n" * 240 for i in range(260)]
    for name in names:
        os.link(base / "keep", base / name)
    data = b"open after removal\n" * 4096
    (base / "final").write_bytes(data)
    (base / "pressure").touch()
    (base / "empty").mkdir()
    fd = os.open(base / "final", os.O_RDONLY)
    mapped = mmap.mmap(fd, len(data), access=mmap.ACCESS_READ)
    sync(base)
    enospc.main(str(base / "full"))
    # Consume the smaller write reservation margin as well.
    with (base / "pressure").open("r+b", buffering=0) as stream:
        for i in range(8192):
            try:
                assert stream.write(bytes(4096)) == 4096
            except OSError as error:
                assert error.errno == errno.ENOSPC, error
                break
            if i % 128 == 127:
                os.fsync(stream.fileno())
        else:
            raise AssertionError("use a small disposable image")
        os.fsync(stream.fileno())
    print(f"write margin exhausted after {i} sectors", flush=True)
    os.unlink(base / "alias")
    for name in [*COLLISIONS, *names]:
        os.unlink(base / name)
    assert os.stat(base / "keep").st_nlink == 1
    os.unlink(base / "final")
    assert os.fstat(fd).st_nlink == 0
    assert os.pread(fd, len(data), 0) == mapped[:] == data
    os.rmdir(base / "empty")
    mapped.close()
    os.close(fd)
    print("packed links and open final-link cleanup passed", flush=True)
    # Free namespace storage and prove the ordinary allocator can reuse it.
    for directory in sorted((base / "full").iterdir())[:16]:
        for path in directory.iterdir():
            path.unlink()
        directory.rmdir()
    sync(base)
    (base / "reused").write_bytes(b"reused\n")
    sync(base)
    capacity_verify(base)
    assert not os.statvfs(base).f_flag & os.ST_RDONLY
    print("protected unlink at metadata exhaustion passed", flush=True)


def capacity_verify(base):
    assert (base / "keep").read_bytes() == b"capacity\n"
    assert (base / "keep").stat().st_nlink == 1
    assert (base / "reused").read_bytes() == b"reused\n"
    assert {p.name for p in base.iterdir()} == {
        "keep", "pressure", "full", "reused"}
    print("capacity namespace verified", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in (
            "create", "verify", "readonly", "capacity", "capacity_verify"):
        sys.exit(f"usage: {sys.argv[0]} "
                 "create|verify|readonly|capacity|capacity_verify directory")
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
