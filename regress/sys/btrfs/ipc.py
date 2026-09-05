#!/usr/bin/env python3
"""FIFO/socket creation, IPC, and persistence on a disposable filesystem."""
import errno
import os
import select
import socket
import stat
import sys

from namespace import COLLISIONS, child_checks, expect_error, wait


def pipe_io():
    before = os.stat("pipe")
    os.truncate("pipe", 8193)
    os.truncate("pipe", 0)
    after = os.stat("pipe")
    assert (before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    expect_error(errno.ENXIO, os.open, "pipe", os.O_WRONLY | os.O_NONBLOCK)
    reader = os.open("pipe", os.O_RDONLY | os.O_NONBLOCK)
    assert os.read(reader, 1) == b""
    writer = os.open("alias", os.O_WRONLY | os.O_NONBLOCK)
    os.ftruncate(writer, 12345)
    expect_error(errno.EAGAIN, os.read, reader, 1)
    assert os.fpathconf(writer, "PC_PIPE_BUF") >= 512
    queue = select.kqueue()
    try:
        queue.control([select.kevent(reader, filter=select.KQ_FILTER_READ,
                                     flags=select.KQ_EV_ADD)], 0)
        assert queue.control([], 1, 0) == []
        assert os.write(writer, b"pipe payload") == 12
        events = queue.control([], 1, 2)
        assert len(events) == 1 and events[0].data == 12
        assert os.read(reader, 100) == b"pipe payload"
        os.close(writer)
        assert os.read(reader, 1) == b""
    finally:
        queue.close()
    os.close(reader)

    # Blocking opens must release the vnode lock for the peer.
    def send():
        fd = os.open("alias", os.O_WRONLY)
        assert os.write(fd, b"blocking") == 8
        os.close(fd)

    pid = child_checks(send)
    fd = os.open("pipe", os.O_RDONLY)
    assert os.read(fd, 100) == b"blocking"
    assert os.read(fd, 1) == b""
    os.close(fd)
    wait(pid)


def socket_io():
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind("socket")
        server.listen(1)
        os.link("socket", "socket-alias")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.connect("socket-alias")
            peer, _ = server.accept()
            with peer:
                client.sendall(b"request")
                assert peer.recv(100) == b"request"
                peer.sendall(b"reply")
                assert client.recv(100) == b"reply"


def verify():
    info = os.stat("pipe")
    assert stat.S_ISFIFO(info.st_mode)
    assert (stat.S_IMODE(info.st_mode), info.st_nlink) == (0o640, 2)
    assert (info.st_size, info.st_blocks) == (0, 0)
    assert os.stat("alias").st_ino == info.st_ino
    for name in COLLISIONS:
        assert stat.S_ISFIFO(os.stat(name).st_mode)
    info = os.stat("socket")
    assert stat.S_ISSOCK(info.st_mode)
    assert info.st_nlink == 2 and info.st_size == 0
    assert os.stat("socket-alias").st_ino == info.st_ino
    assert os.stat(".").st_size == sum(len(n) * 2 for n in os.listdir("."))
    pipe_io()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        # Even read-only mounts must get past VOP_ACCESS to the stale socket.
        expect_error(errno.ECONNREFUSED, client.connect, "socket")


def create():
    os.umask(0o027)
    os.mkfifo("pipe", 0o666)
    os.link("pipe", "alias")
    for name in COLLISIONS:
        os.mkfifo(name)
        fd = os.open(".", os.O_RDONLY)
        os.fsync(fd)
        os.close(fd)
    expect_error(errno.EEXIST, os.mkfifo, "pipe")
    os.mkdir("denied", 0o700)

    def denied():
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
        expect_error(errno.EACCES, os.mkfifo, "denied/pipe")

    wait(child_checks(denied))
    socket_io()
    verify()
    fd = os.open(".", os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase == "create":
        os.mkdir(directory)
    os.chdir(directory)
    if phase == "create":
        create()
    else:
        verify()
        if phase == "readonly":
            expect_error(errno.EROFS, os.mkfifo, "readonly-new")
            expect_error(errno.EROFS, os.chmod, "pipe", 0o600)
            with socket.socket(socket.AF_UNIX) as sock:
                expect_error(errno.EROFS, sock.bind, "readonly-socket")
    print(f"IPC {phase} passed", flush=True)
