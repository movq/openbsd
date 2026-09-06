#!/usr/bin/env python3
#
# Kqueue readiness and vnode notifications on a disposable filesystem.
#
import errno
import os
import select
import signal
import subprocess
import sys
from contextlib import closing

from namespace import child_checks, expect_error, put, wait


READ = select.KQ_FILTER_READ
WRITE = select.KQ_FILTER_WRITE
VNODE = select.KQ_FILTER_VNODE
ADD = select.KQ_EV_ADD | select.KQ_EV_CLEAR
NOTE_EOF = 0x0002  # OpenBSD EVFILT_READ flag, absent from Python's constants.


def register(queue, fd, kind, notes=0):
    queue.control([select.kevent(fd, filter=kind, flags=ADD,
                                fflags=notes)], 0, 0)


def event(queue, fd, kind, notes=None):
    events = queue.control(None, 8, 5)
    assert len(events) == 1, events
    result = events[0]
    assert (result.ident, result.filter) == (fd, kind), result
    assert not result.flags & select.KQ_EV_ERROR, result
    if notes is not None:
        assert result.fflags == notes, result
    return result


def quiet(queue):
    assert queue.control(None, 8, 0) == []


def readiness():
    fd = os.open("file", os.O_RDONLY)
    size = os.fstat(fd).st_size
    with closing(select.kqueue()) as queue:
        register(queue, fd, READ)
        assert event(queue, fd, READ).data == size
        os.lseek(fd, 3, os.SEEK_SET)
        register(queue, fd, READ)
        assert event(queue, fd, READ).data == size - 3
        os.lseek(fd, 0, os.SEEK_END)
        register(queue, fd, READ)
        quiet(queue)
        # poll/select report a regular file readable even at EOF.
        assert select.select([fd], [], [], 0)[0] == [fd]
        poller = select.poll()
        poller.register(fd, select.POLLIN)
        assert poller.poll(0) == [(fd, select.POLLIN)]
        register(queue, fd, READ, NOTE_EOF)
        assert event(queue, fd, READ, NOTE_EOF).data == 0
        queue.control([select.kevent(fd, filter=READ,
                       flags=select.KQ_EV_DELETE)], 0, 0)
        quiet(queue)
    with closing(select.kqueue()) as queue:
        register(queue, fd, WRITE)
        event(queue, fd, WRITE)
        # Closing the descriptor detaches its knotes.
        os.close(fd)
        quiet(queue)


def notifications():
    fd = os.open("file", os.O_RDWR)
    directory = os.open(".", os.O_RDONLY)
    with closing(select.kqueue()) as queue:
        register(queue, fd, VNODE, select.KQ_NOTE_WRITE |
                 select.KQ_NOTE_EXTEND | select.KQ_NOTE_ATTRIB)
        quiet(queue)
        os.pwrite(fd, b"X", 0)
        event(queue, fd, VNODE, select.KQ_NOTE_WRITE)
        quiet(queue)
        os.pwrite(fd, b"tail", os.fstat(fd).st_size)
        event(queue, fd, VNODE, select.KQ_NOTE_WRITE | select.KQ_NOTE_EXTEND)
        quiet(queue)
        os.fchmod(fd, 0o600)
        event(queue, fd, VNODE, select.KQ_NOTE_ATTRIB)
        quiet(queue)
        # A failed operation must not generate a mutation event.
        expect_error(errno.EINVAL, os.ftruncate, fd, -1)
        quiet(queue)
        # A subscription to ATTRIB alone ignores data-write hints.
        register(queue, fd, VNODE, select.KQ_NOTE_ATTRIB)
        os.pwrite(fd, b"Y", 1)
        quiet(queue)
        os.utime("file", ns=(1_000_000_000, 2_000_000_000))
        event(queue, fd, VNODE, select.KQ_NOTE_ATTRIB)
    with closing(select.kqueue()) as queue:
        register(queue, directory, VNODE, select.KQ_NOTE_WRITE)
        put("created", b"directory event\n")
        event(queue, directory, VNODE, select.KQ_NOTE_WRITE)
        os.mkdir("subdir")
        event(queue, directory, VNODE, select.KQ_NOTE_WRITE)
        os.symlink("file", "link")
        event(queue, directory, VNODE, select.KQ_NOTE_WRITE)
        expect_error(errno.EEXIST, os.mkdir, "subdir")
        quiet(queue)
    os.close(fd)
    os.close(directory)


def blocking_read():
    fd = os.open("file", os.O_RDONLY)
    os.lseek(fd, 0, os.SEEK_END)
    ready_r, ready_w = os.pipe()
    with closing(select.kqueue()) as queue:
        register(queue, fd, READ)
        quiet(queue)

        def writer():
            queue.close()
            os.close(fd)
            os.close(ready_w)
            assert os.read(ready_r, 1) == b"w"
            other = os.open("file", os.O_WRONLY | os.O_APPEND)
            assert os.write(other, b"wake") == 4
            os.close(other)

        pid = child_checks(writer)
        os.close(ready_r)
        os.write(ready_w, b"w")
        result = event(queue, fd, READ)
        assert result.data == 4, result
        assert os.read(fd, 4) == b"wake"
        quiet(queue)
        wait(pid)
    os.close(fd)
    os.close(ready_w)


def revoke(mountpoint):
    name = os.path.join(mountpoint, "kqueue-revoke")
    put(name, b"revoke\n")
    fd = os.open(name, os.O_RDWR)
    os.lseek(fd, 0, os.SEEK_END)
    unrequested = os.open(name, os.O_RDONLY)
    with closing(select.kqueue()) as queue:
        register(queue, fd, READ)
        register(queue, fd, WRITE)
        register(queue, fd, VNODE, select.KQ_NOTE_REVOKE)
        register(queue, unrequested, VNODE)
        event(queue, fd, WRITE)
        quiet(queue)
        subprocess.run(["umount", "-f", mountpoint], check=True)
        events = queue.control(None, 8, 5)
        assert {(e.ident, e.filter) for e in events} == {
            (fd, READ), (fd, WRITE), (fd, VNODE), (unrequested, VNODE)}
        for result in events:
            assert result.flags & select.KQ_EV_EOF, result
            if result.filter in (READ, WRITE):
                assert result.flags & select.KQ_EV_ONESHOT, result
            elif result.ident == fd:
                assert result.fflags == select.KQ_NOTE_REVOKE, result
        # Subsequent scans must not access the reclaimed inode.
        quiet(queue)
        os.close(fd)
        os.close(unrequested)
        quiet(queue)
    print("kqueue revoke verification passed", flush=True)


if __name__ == "__main__":
    signal.alarm(45)
    if len(sys.argv) != 3 or sys.argv[1] not in ("create", "readonly", "revoke"):
        sys.exit(f"usage: {sys.argv[0]} create new-test-directory\n"
                 f"       {sys.argv[0]} readonly existing-test-directory\n"
                 f"       {sys.argv[0]} revoke mountpoint")
    if sys.argv[1] == "revoke":
        revoke(os.path.abspath(sys.argv[2]))
        sys.exit(0)
    if sys.argv[1] == "create":
        os.mkdir(sys.argv[2])
    os.chdir(sys.argv[2])
    if sys.argv[1] == "create":
        put("file", b"initial payload\n")
    readiness()
    if sys.argv[1] == "create":
        notifications()
        blocking_read()
    print("kqueue verification passed", flush=True)
