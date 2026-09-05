#!/usr/bin/env python3
#
# Advisory locking on a disposable mounted filesystem.  Run as root.
#
import errno
import fcntl
import os
import select
import signal
import struct
import subprocess
import sys

from namespace import child_checks, expect_error, put, wait


def conflict(function, *args):
    try:
        function(*args)
    except OSError as error:
        assert error.errno in (errno.EAGAIN, errno.EACCES), error
    else:
        raise AssertionError("conflicting lock succeeded")


def record_locks():
    fd = os.open("file", os.O_RDWR)
    fcntl.lockf(fd, fcntl.LOCK_EX, 20, 10)

    def contender():
        other = os.open("file", os.O_RDWR)
        conflict(fcntl.lockf, other, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, 10)
        conflict(fcntl.lockf, other, fcntl.LOCK_SH | fcntl.LOCK_NB, 1, 29)
        # Adjacent ranges do not conflict.
        fcntl.lockf(other, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 0)
        fcntl.lockf(other, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 30)
        request = struct.pack("@qqihh", 10, 20, 0, fcntl.F_WRLCK, os.SEEK_SET)
        result = fcntl.fcntl(other, fcntl.F_GETLK, request)
        start, length, pid, kind, whence = struct.unpack("@qqihh", result)
        assert (start, length, pid, kind, whence) == \
            (10, 20, os.getppid(), fcntl.F_WRLCK, os.SEEK_SET)
        os.close(other)

    wait(child_checks(contender))
    # Split the range with an unlock, then confirm the hole is available.
    fcntl.lockf(fd, fcntl.LOCK_UN, 5, 15)

    def split():
        other = os.open("file", os.O_RDWR)
        fcntl.lockf(other, fcntl.LOCK_EX | fcntl.LOCK_NB, 5, 15)
        conflict(fcntl.lockf, other, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, 14)
        conflict(fcntl.lockf, other, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, 20)
        os.close(other)

    wait(child_checks(split))
    # POSIX locks are released by closing any descriptor for that inode.
    os.close(os.open("file", os.O_RDONLY))

    def unlocked():
        other = os.open("file", os.O_RDWR)
        fcntl.lockf(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.close(other)

    wait(child_checks(unlocked))
    # SEEK_END must use the current inode size.
    fcntl.lockf(fd, fcntl.LOCK_EX, 0, -8, os.SEEK_END)

    def end_range():
        other = os.open("file", os.O_RDWR)
        end = os.fstat(other).st_size
        fcntl.lockf(other, fcntl.LOCK_EX | fcntl.LOCK_NB, end - 8, 0)
        conflict(fcntl.lockf, other, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, end - 8)
        os.close(other)

    wait(child_checks(end_range))
    os.close(fd)


def blocking(use_flock):
    fd = os.open("file", os.O_RDWR)
    lock = fcntl.flock if use_flock else fcntl.lockf
    lock(fd, fcntl.LOCK_EX)
    ready_r, ready_w = os.pipe()
    done_r, done_w = os.pipe()

    def contender():
        os.close(fd)  # Drop the inherited flock open-file description.
        os.close(ready_r)
        os.close(done_r)
        other = os.open("file", os.O_RDWR)
        conflict(lock, other, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.write(ready_w, b"r")
        lock(other, fcntl.LOCK_EX)
        os.write(done_w, b"d")
        os.close(other)

    pid = child_checks(contender)
    os.close(ready_w)
    os.close(done_w)
    assert os.read(ready_r, 1) == b"r"
    assert not select.select([done_r], [], [], 0.2)[0]
    lock(fd, fcntl.LOCK_UN)
    assert select.select([done_r], [], [], 5)[0], "waiter did not wake"
    assert os.read(done_r, 1) == b"d"
    wait(pid)
    for item in (fd, ready_r, done_r):
        os.close(item)


def flock_lifetime():
    fd = os.open("file", os.O_RDONLY)
    fcntl.flock(fd, fcntl.LOCK_SH)

    def shared():
        os.close(fd)
        other = os.open("file", os.O_RDONLY)
        fcntl.flock(other, fcntl.LOCK_SH | fcntl.LOCK_NB)
        conflict(fcntl.flock, other, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.close(other)

    wait(child_checks(shared))
    fcntl.flock(fd, fcntl.LOCK_EX)
    duplicate = os.dup(fd)
    os.close(fd)

    def separate_open():
        os.close(duplicate)
        other = os.open("file", os.O_RDONLY)
        conflict(fcntl.flock, other, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.close(other)

    wait(child_checks(separate_open))
    os.close(duplicate)
    other = os.open("file", os.O_RDONLY)
    fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.close(other)


def exit_release(use_flock):
    lock = fcntl.flock if use_flock else fcntl.lockf
    ready_r, ready_w = os.pipe()

    def holder():
        fd = os.open("file", os.O_RDWR)
        lock(fd, fcntl.LOCK_EX)
        os.write(ready_w, b"r")
        signal.pause()

    pid = child_checks(holder)
    os.close(ready_w)
    assert os.read(ready_r, 1) == b"r"
    os.kill(pid, signal.SIGKILL)
    assert os.WTERMSIG(os.waitpid(pid, 0)[1]) == signal.SIGKILL
    fd = os.open("file", os.O_RDWR)
    lock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.close(fd)
    os.close(ready_r)


def reclaim(mountpoint):
    # Force reclaim with both held locks and sleeping requests.  No process
    # may keep its current directory inside the mount during this test.
    requests = []
    for use_flock in (False, True):
        name = os.path.join(mountpoint, "lock-reclaim-" + str(use_flock))
        put(name, b"reclaim locks\n")
        lock = fcntl.flock if use_flock else fcntl.lockf
        fd = os.open(name, os.O_RDWR)
        lock(fd, fcntl.LOCK_EX)
        ready_r, ready_w = os.pipe()
        done_r, done_w = os.pipe()

        def waiter():
            for _, inherited, _, _ in requests:
                os.close(inherited)
            os.close(fd)
            os.close(done_r)
            other = os.open(name, os.O_RDWR)
            conflict(lock, other, fcntl.LOCK_EX | fcntl.LOCK_NB)
            os.write(ready_w, b"r")
            try:
                lock(other, fcntl.LOCK_EX)
            except OSError as error:
                # Python may retry EINTR against the now-dead vnode.
                assert error.errno in (errno.EINTR, errno.EBADF), error
            else:
                raise AssertionError("lock succeeded after forced reclaim")
            os.write(done_w, b"d")
            os.close(other)

        pid = child_checks(waiter)
        os.close(ready_w)
        os.close(done_w)
        assert os.read(ready_r, 1) == b"r"
        assert not select.select([done_r], [], [], 0.2)[0]
        requests.append((pid, fd, ready_r, done_r))
    subprocess.run(["umount", "-f", mountpoint], check=True)
    for pid, fd, ready_r, done_r in requests:
        assert select.select([done_r], [], [], 5)[0], "reclaim did not wake"
        assert os.read(done_r, 1) == b"d"
        wait(pid)
        for item in (fd, ready_r, done_r):
            os.close(item)
    print("forced lock reclaim verification passed", flush=True)


def readonly(directory):
    os.chdir(directory)
    flock_lifetime()
    fd = os.open("file", os.O_RDONLY)
    fcntl.lockf(fd, fcntl.LOCK_SH)

    def reader():
        other = os.open("file", os.O_RDONLY)
        fcntl.lockf(other, fcntl.LOCK_SH | fcntl.LOCK_NB)
        os.close(other)

    wait(child_checks(reader))
    os.close(fd)
    print("read-only advisory locking verification passed", flush=True)


if __name__ == "__main__":
    signal.alarm(45)
    if len(sys.argv) == 3 and sys.argv[1] in ("reclaim", "readonly"):
        if sys.argv[1] == "reclaim":
            reclaim(os.path.abspath(sys.argv[2]))
        else:
            readonly(sys.argv[2])
        sys.exit(0)
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} new-test-directory\n"
                 f"       {sys.argv[0]} reclaim mountpoint\n"
                 f"       {sys.argv[0]} readonly existing-test-directory")
    os.mkdir(sys.argv[1])
    os.chdir(sys.argv[1])
    put("file", bytes(128))
    # Opening through a symlink must reach the same vnode's lock state.
    os.symlink("file", "alias")
    fd = os.open("alias", os.O_RDWR)
    fcntl.flock(fd, fcntl.LOCK_EX)
    other = os.open("file", os.O_RDWR)
    conflict(fcntl.flock, other, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.close(other)
    os.close(fd)
    record_locks()
    flock_lifetime()
    for use_flock in (False, True):
        blocking(use_flock)
        exit_release(use_flock)
    print("advisory locking verification passed", flush=True)
