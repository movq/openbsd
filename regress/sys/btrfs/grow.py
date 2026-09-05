#!/usr/bin/env python3
"""Sparse truncate growth: seed on host, exercise in VM, check and remount."""
import errno
import mmap
import os
from pathlib import Path
import resource
import select
import signal
import stat
import sys

from namespace import child_checks, expect_error, put, wait


SIZES = (0, 1, 100, 2048, 4096, 6003, 65539)
COMPRESSED_SIZES = (0, 1, 100, 2048, 6003, 8192, 65539)
FINAL = 131079


def original(size):
    return bytes((n * 37 + 19) % 251 for n in range(size))


def seed(sizes=SIZES):
    for size in sizes:
        Path(f"import-{size}").write_bytes(original(size))
    Path("capacity-inline").write_bytes(original(100))
    Path("capacity-regular").write_bytes(original(6003))


def seed_compressed():
    # btrfs-progs 7.0 can mark a one-sector regular extent compressed while
    # storing raw data. Use two sectors for the aligned compressed fixture.
    seed(COMPRESSED_SIZES)


def state(name):
    info = os.stat(name)
    return (info.st_size, info.st_blocks, info.st_mode,
            info.st_mtime_ns, info.st_ctime_ns)


def fail(name, code, function, *args):
    before = state(name)
    expect_error(code, function, *args)
    assert state(name) == before
    assert not os.statvfs(".").f_flag & os.ST_RDONLY


def grow_case(name, size):
    fd = os.open(name, os.O_RDWR)
    assert os.read(fd, size + 1) == original(size)
    if size:
        mapping = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
        assert mapping[:] == original(size)
        mapping.close()
    original_blocks = os.fstat(fd).st_blocks
    # Repeated extension tests the partially zeroed EOF sector, then holes.
    for length in (size + 1, size + 7, size + 4096, FINAL):
        os.ftruncate(fd, length)
        assert os.lseek(fd, 0, os.SEEK_CUR) == size
        assert os.pread(fd, length + 1, 0) == (
            original(size) + bytes(length - size)), (name, length)
    os.fsync(fd)
    assert os.fstat(fd).st_blocks <= max(original_blocks, 8)
    mapping = mmap.mmap(fd, FINAL, access=mmap.ACCESS_READ)
    assert mapping[:] == original(size) + bytes(FINAL - size)
    mapping.close()
    os.ftruncate(fd, FINAL)
    fail(name, errno.EOPNOTSUPP, os.ftruncate, fd, 0)
    assert os.pwrite(fd, b"tail", FINAL - 4) == 4
    os.fsync(fd)
    os.close(fd)


def policy():
    put("policy", b"data")
    fd = os.open("policy", os.O_RDWR)
    for flag in (stat.UF_IMMUTABLE, stat.UF_APPEND):
        os.chflags("policy", flag)
        fail("policy", errno.EPERM, os.ftruncate, fd, 8192)
    os.chflags("policy", 0)
    fail("policy", errno.EINVAL, os.ftruncate, fd, -1)
    os.close(fd)
    fd = os.open("policy", os.O_RDONLY)
    fail("policy", errno.EINVAL, os.ftruncate, fd, 8192)
    os.close(fd)
    os.chown("policy", 1234, 1234)
    os.chmod("policy", 0o6755)
    put("denied", b"root")
    os.chmod("denied", 0o600)

    def owner():
        os.setgroups([])
        os.setgid(1234)
        os.setuid(1234)
        fail("denied", errno.EACCES, os.truncate, "denied", 8192)
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))
        fail("policy", errno.EFBIG, os.truncate, "policy", 8192)
        os.truncate("policy", 4096)
        assert os.stat("policy").st_mode & 0o6000 == 0

    wait(child_checks(owner))
    assert Path("policy").read_bytes() == b"data" + bytes(4092)


def notifications():
    put("events", b"event")
    fd = os.open("events", os.O_RDWR)
    queue = select.kqueue()
    queue.control([select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                   flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                   fflags=select.KQ_NOTE_ATTRIB | select.KQ_NOTE_EXTEND |
                   select.KQ_NOTE_WRITE)], 0, 0)
    os.ftruncate(fd, 8192)
    events = queue.control(None, 4, 5)
    assert len(events) == 1
    assert events[0].fflags == select.KQ_NOTE_ATTRIB | select.KQ_NOTE_EXTEND
    fail("events", errno.EOPNOTSUPP, os.ftruncate, fd, 1)
    assert queue.control(None, 4, 0) == []
    queue.close()
    os.close(fd)


def exercise():
    for size in SIZES:
        put(f"created-{size}", original(size))
    os.link("import-100", "alias")
    jobs = [child_checks(lambda name=f"{kind}-{size}", size=size:
                         grow_case(name, size))
            for kind in ("import", "created") for size in SIZES]
    for pid in jobs:
        wait(pid)
    # Growth and ordered writes on the same inode without intervening fsync.
    put("ordered", b"start")
    fd = os.open("ordered", os.O_RDWR)
    os.ftruncate(fd, 17)
    os.pwrite(fd, b"middle", 7)
    os.ftruncate(fd, 8199)
    os.pwrite(fd, b"end", 8196)
    os.fsync(fd)
    os.close(fd)
    # Very large growth must consume metadata only.
    put("huge", b"")
    os.truncate("huge", (1 << 40) + 7)
    assert os.stat("huge").st_blocks == 0
    fd = os.open("huge", os.O_RDONLY)
    assert os.pread(fd, 8, (1 << 40) - 1) == bytes(8)
    os.close(fd)
    policy()
    notifications()
    os.sync()
    verify()


def verify():
    for kind in ("import", "created"):
        for size in SIZES:
            expected = original(size) + bytes(FINAL - size - 4) + b"tail"
            assert Path(f"{kind}-{size}").read_bytes() == expected
    assert Path("alias").read_bytes() == Path("import-100").read_bytes()
    assert os.stat("alias").st_ino == os.stat("import-100").st_ino
    assert Path("ordered").read_bytes() == (
        b"start" + bytes(2) + b"middle" + bytes(8183) + b"end")
    assert os.stat("huge").st_size == (1 << 40) + 7
    assert os.stat("huge").st_blocks == 0


def readonly():
    verify()
    for name in ("import-100", "created-6003", "huge"):
        fail_size = os.stat(name).st_size + 1
        before = state(name)
        expect_error(errno.EROFS, os.truncate, name, fail_size)
        assert state(name) == before


def reject():
    # mkfs leaves small incompressible inputs uncompressed.  These cases
    # cover compressed inline data and unaligned regular EOF sectors.
    for size in (2048, 6003, 65539):
        name = f"import-{size}"
        fail(name, errno.EOPNOTSUPP, os.truncate, name, size + 8192)
    os.sync()


def compressed():
    # Verify the compressed prefix independently with host-side restore.
    reject()
    os.truncate("import-8192", 12288)
    fd = os.open("import-8192", os.O_RDWR)
    os.pwrite(fd, b"tail", 12284)
    os.fsync(fd)
    os.close(fd)
    # Preserve the imported compression policy across chflags.
    os.chflags("import-6003", stat.UF_NODUMP)
    os.sync()


def verify_compressed():
    for size in COMPRESSED_SIZES:
        expected = original(size)
        if size == 8192:
            expected += bytes(4092) + b"tail"
        assert Path(f"import-{size}").read_bytes() == expected


def zstd():
    verify_compressed_seed()
    # Writes into compressed inline/regular mappings must reject before
    # changing the inode or the surrounding open transaction.
    for size in (2048, 6003, 8192, 65539):
        name = f"import-{size}"
        fd = os.open(name, os.O_RDWR)
        for offset in (0, size // 2, size - 1):
            put(f"survivor-{size}-{offset}", b"pending")
            fail(name, errno.EOPNOTSUPP, os.pwrite, fd, b"fail", offset)
            assert Path(name).read_bytes() == original(size)
            os.fsync(fd)
        os.close(fd)
    # A distant write cannot mix inline and regular mappings, either.
    fd = os.open("import-2048", os.O_RDWR)
    fail("import-2048", errno.EOPNOTSUPP, os.pwrite, fd, b"fail", 16384)
    os.close(fd)
    compressed()
    verify_compressed()
    os.link("import-6003", "compressed-alias")
    os.chmod("compressed-alias", 0o640)
    assert os.stat("compressed-alias").st_flags == stat.UF_NODUMP
    assert Path("compressed-alias").read_bytes() == original(6003)
    # Once the tail is uncompressed, ordinary COW writes may replace it.
    fd = os.open("import-8192", os.O_RDWR)
    os.pwrite(fd, b"TAIL", 12284)
    os.fsync(fd)
    os.pwrite(fd, b"tail", 12284)
    os.fsync(fd)
    os.close(fd)
    os.sync()
    verify_zstd()


def verify_zstd():
    verify_compressed()
    assert os.stat("compressed-alias").st_ino == os.stat("import-6003").st_ino
    assert Path("compressed-alias").read_bytes() == original(6003)
    for size in (2048, 6003, 8192, 65539):
        for offset in (0, size // 2, size - 1):
            assert Path(f"survivor-{size}-{offset}").read_bytes() == b"pending"


def verify_seed(sizes=SIZES):
    for size in sizes:
        assert Path(f"import-{size}").read_bytes() == original(size)
    assert Path("capacity-inline").read_bytes() == original(100)
    assert Path("capacity-regular").read_bytes() == original(6003)


def verify_compressed_seed():
    verify_seed(COMPRESSED_SIZES)


def capacity():
    put("sparse-capacity", b"")
    fd = os.open("filler", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    for sector in range(32768):
        try:
            os.pwrite(fd, b"F" * 4096, sector * 4096)
        except OSError as error:
            assert error.errno == errno.ENOSPC
            break
    else:
        raise AssertionError("use a small existing data block group")
    os.fsync(fd)
    os.close(fd)
    for name in ("capacity-inline", "capacity-regular"):
        fail(name, errno.ENOSPC, os.truncate, name, FINAL)
    verify_seed()
    # A data reservation is unnecessary for a hole, even with data full.
    os.truncate("sparse-capacity", FINAL)
    assert Path("sparse-capacity").read_bytes() == bytes(FINAL)
    assert os.stat("sparse-capacity").st_blocks == 0
    os.sync()
    assert not os.statvfs(".").f_flag & os.ST_RDONLY


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase in ("seed", "seed_compressed"):
        os.makedirs(directory)
    os.chdir(directory)
    globals()[phase]()
    print("grow", phase, "ok", flush=True)
