#!/usr/bin/env python3
"""Atomic shrinking, pending-sector cancellation, and truncate/regrow caches."""
import errno
import mmap
import os
from pathlib import Path
import select
import stat
import sys

from namespace import child_checks, expect_error, put, wait


SIZES = (1, 100, 2048, 4096, 6003, 98311)
FINAL = 200003


def original(size):
    return bytes((n * 37 + 19) % 251 for n in range(size))


def seed():
    for size in SIZES:
        for style in ("zero", "partial", "aligned"):
            Path(f"import-{size}-{style}").write_bytes(original(size))
    Path("split").write_bytes(original(196615))


def target(size, style):
    return {"zero": 0, "partial": size // 2,
            "aligned": (size // 8192) * 4096}[style]


def case(name, size, style):
    fd = os.open(name, os.O_RDWR)
    length = target(size, style)
    data = original(size)
    assert os.read(fd, size + 1) == data
    # Keep mapped pages live through both shrink and growth.
    mapping = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
    assert mapping[:] == data
    os.ftruncate(fd, length)
    assert os.lseek(fd, 0, os.SEEK_CUR) == size
    assert os.pread(fd, size + 1, 0) == data[:length]
    assert mapping[:length] == data[:length]
    if length == 0:
        assert os.fstat(fd).st_blocks == 0
    os.ftruncate(fd, FINAL)
    expected = data[:length] + bytes(FINAL - length)
    assert mapping[:] == expected[:size]
    mapping.close()
    assert os.pread(fd, FINAL + 1, 0) == expected
    os.fsync(fd)
    assert os.pread(fd, FINAL + 1, 0) == expected
    os.close(fd)


def ordered():
    # Reuse the same logical offsets before commit, with a mixture of
    # committed and pending sectors. Exercise cancellation and partial tails.
    put("ordered", original(65536))
    fd = os.open("ordered", os.O_RDWR)
    os.fsync(fd)
    expected = bytearray(original(65536))
    for length in (24579, 8192, 1, 0, 12301, 3, 0):
        os.pwrite(fd, b"W" * 8192, 4096)
        if len(expected) < 12288:
            expected.extend(bytes(12288 - len(expected)))
        expected[4096:12288] = b"W" * 8192
        os.ftruncate(fd, length)
        del expected[length:]
        if len(expected) < length:
            expected.extend(bytes(length - len(expected)))
        assert os.pread(fd, 65537, 0) == expected
        os.ftruncate(fd, 32768)
        expected.extend(bytes(32768 - len(expected)))
        assert os.pread(fd, 65537, 0) == expected
    assert expected == bytes(32768)
    os.fsync(fd)
    assert os.fstat(fd).st_blocks == 0
    os.close(fd)
    # O_TRUNC must preserve inode identity and hard links.
    os.link("ordered", "alias")
    Path("ordered").write_bytes(b"final")
    assert Path("alias").read_bytes() == b"final"


def split():
    fd = os.open("split", os.O_RDWR)
    expected = bytearray(original(196615))
    # Multiple mappings share one data ownership reference after these COWs.
    for sector in (0, 3, 5, 10, 20, 30, 40):
        os.pwrite(fd, b"S" * 4096, sector * 4096)
        expected[sector * 4096:(sector + 1) * 4096] = b"S" * 4096
    os.fsync(fd)
    for length in (160003, 98304, 40961, 12288, 4103, 17, 0):
        os.ftruncate(fd, length)
        del expected[length:]
        assert os.pread(fd, 200000, 0) == expected
        os.fsync(fd)
    assert os.fstat(fd).st_blocks == 0
    os.close(fd)


def policy():
    put("policy", b"policy")
    fd = os.open("policy", os.O_RDWR)
    for flag in (stat.UF_IMMUTABLE, stat.UF_APPEND):
        os.chflags("policy", flag)
        before = os.fstat(fd)
        expect_error(errno.EPERM, os.ftruncate, fd, 0)
        after = os.fstat(fd)
        assert before == after
    os.chflags("policy", 0)
    queue = select.kqueue()
    queue.control([select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                   flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                   fflags=select.KQ_NOTE_ATTRIB | select.KQ_NOTE_EXTEND |
                   select.KQ_NOTE_WRITE)], 0, 0)
    os.ftruncate(fd, 3)
    events = queue.control(None, 4, 5)
    assert len(events) == 1
    assert events[0].fflags == select.KQ_NOTE_ATTRIB
    queue.close()
    os.close(fd)


def exercise():
    for size in SIZES:
        for style in ("zero", "partial", "aligned"):
            put(f"created-{size}-{style}", original(size))
    # Parallel inode mutations and commits share allocation/checksum trees.
    for size in SIZES:
        if size == max(SIZES):
            # Exercise fragmented shrinking alongside imported mappings.
            for kind in ("import", "created"):
                for style in ("zero", "partial", "aligned"):
                    case(f"{kind}-{size}-{style}", size, style)
            continue
        jobs = [child_checks(lambda name=f"{kind}-{size}-{style}",
                             style=style: case(name, size, style))
                for kind in ("import", "created")
                for style in ("zero", "partial", "aligned")]
        for pid in jobs:
            wait(pid)
    ordered()
    split()
    policy()
    put("huge", b"")
    os.truncate("huge", (1 << 40) + 7)
    os.truncate("huge", 3)
    assert os.stat("huge").st_blocks == 0
    sparse()
    os.sync()
    verify()


def sparse():
    # Middle-hole replacement must use rounded EOF, even when the inode's
    # size ends part way through a later sector.
    put("sparse-middle", b"")
    os.truncate("sparse-middle", 65539)
    fd = os.open("sparse-middle", os.O_RDWR)
    for offset in (0, 8195, 4097, 32768):
        assert os.pwrite(fd, b"middle", offset) == 6
    os.fsync(fd)
    os.close(fd)
    verify_sparse()


def verify_sparse():
    data = bytearray(65539)
    for offset in (0, 8195, 4097, 32768):
        data[offset:offset + 6] = b"middle"
    assert Path("sparse-middle").read_bytes() == data


def verify():
    for size in SIZES:
        for kind in ("import", "created"):
            for style in ("zero", "partial", "aligned"):
                length = target(size, style)
                assert Path(f"{kind}-{size}-{style}").read_bytes() == (
                    original(size)[:length] + bytes(FINAL - length))
    assert Path("split").read_bytes() == b""
    assert os.stat("split").st_blocks == 0
    assert Path("ordered").read_bytes() == b"final"
    assert Path("alias").read_bytes() == b"final"
    assert os.stat("alias").st_ino == os.stat("ordered").st_ino
    assert Path("policy").read_bytes() == b"pol"
    assert Path("huge").read_bytes() == bytes(3)
    verify_sparse()


def readonly():
    verify()
    expect_error(errno.EROFS, os.truncate, "ordered", 0)


def capacity():
    put("sparse", b"")
    os.truncate("sparse", FINAL)
    fd = os.open("filler", os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
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
    # Partial-sector shrink still needs data space.
    for name, length in (("import-100-partial", 17),
                         ("import-6003-partial", 4103)):
        before = os.stat(name)
        data = Path(name).read_bytes()
        expect_error(errno.ENOSPC, os.truncate, name, length)
        assert os.stat(name) == before
        assert Path(name).read_bytes() == data
        assert not os.statvfs(".").f_flag & os.ST_RDONLY
    os.truncate("sparse", 1)
    os.truncate("import-98311-zero", 0)
    assert os.stat("import-98311-zero").st_blocks == 0
    os.truncate("filler", 0)
    assert os.stat("filler").st_blocks == 0
    os.sync()


if __name__ == "__main__":
    phase, directory = sys.argv[1:]
    if phase == "seed":
        os.makedirs(directory)
    os.chdir(directory)
    globals()[phase]()
    print("shrink", phase, "ok", flush=True)
