#!/usr/bin/env python3
"""Disjoint subvolume mounts; root in the VM, seed on the unmounted host."""

import ctypes
import errno
import fcntl
import mmap
import multiprocessing
import os
from pathlib import Path
import subprocess
import sys


def seed(base):
    for name in ("left", "left/nested", "left/frozen", "right"):
        directory = base / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "marker").write_text(name)


def expect_error(expected, operation, *args):
    try:
        operation(*args)
    except OSError as error:
        assert error.errno == expected, error
    else:
        raise AssertionError(f"expected errno {expected}: {operation}")


def payload(label, index):
    return (f"{label}:{index:04d}:".encode() * 2048)[:12288]


def writer(directory, label):
    for index in range(40):
        with (directory / f"file-{index}").open("xb", buffering=0) as output:
            output.write(payload(label, index))
            os.fsync(output.fileno())


def verify_files(directory, label):
    for index in range(40):
        assert (directory / f"file-{index}").read_bytes() == payload(label, index)


def exercise(device, base, ids):
    leftid, nestedid, frozenid, rightid = ids
    left, right, spare = (base / name for name in ("left", "right", "spare"))
    for directory in (left, right, spare):
        directory.mkdir(parents=True, exist_ok=True)
    mounted = set()

    def mount(directory, treeid, readonly=False):
        subprocess.run(["mount_btrfs", "-s", str(treeid), "-o",
                        "ro" if readonly else "rw", device, str(directory)],
                       check=True)
        mounted.add(directory)

    def unmount(directory, force=False):
        subprocess.run(["umount"] + (["-f"] if force else []) +
                       [str(directory)], check=True)
        mounted.remove(directory)

    # Check kernel errno directly, including rejected system-tree IDs.
    class Args(ctypes.Structure):
        _fields_ = [("fspec", ctypes.c_char_p), ("subvolid", ctypes.c_uint64)]

    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
                           ctypes.c_void_p]

    def rejected(treeid, expected, readonly=False):
        args = Args(os.fsencode(device), treeid)
        result = libc.mount(b"btrfs", os.fsencode(spare), int(readonly),
                            ctypes.byref(args))
        if result == 0:
            mounted.add(spare)
            raise AssertionError(f"unexpected mount of {treeid}")
        assert ctypes.get_errno() == expected, (treeid, ctypes.get_errno())

    try:
        for treeid in (1, 2, 6, 255, 2**64 - 1):
            rejected(treeid, errno.EINVAL)
        rejected(1234567, errno.ENOENT)
        rejected(frozenid, errno.EROFS)
        mount(left, leftid)
        mount(right, rightid)
        assert (left / "marker").read_text() == "left"
        assert (right / "marker").read_text() == "right"
        assert left.stat().st_ino == right.stat().st_ino == 256
        assert len({p.stat().st_dev for p in
                    (left, right, left / "nested", left / "frozen")}) == 4
        nested_dev = (left / "nested").stat().st_dev
        for treeid in (0, 5, leftid, rightid, nestedid):
            rejected(treeid, errno.EBUSY)
            rejected(treeid, errno.EBUSY, readonly=True)
        rejected(frozenid, errno.EBUSY, readonly=True)
        assert os.path.samefile(left / "..", base)
        assert os.path.samefile(left / "nested/..", left)
        previous = os.getcwd()
        try:
            os.chdir(left / "nested")
            assert os.getcwd() == str(left / "nested")
            os.chdir("..")
            assert os.getcwd() == str(left)
        finally:
            os.chdir(previous)
        assert "marker" in os.listdir(left)
        expect_error(errno.EROFS, (left / "frozen/new").write_bytes, b"x")
        expect_error(errno.EROFS, os.chmod, left / "frozen/marker", 0o600)
        expect_error(errno.EXDEV, os.link, left / "marker", right / "alias")
        expect_error(errno.EXDEV, os.link, left / "marker",
                     left / "nested/alias")
        workers = [multiprocessing.Process(target=writer, args=(p, label))
                   for p, label in ((left, "left"), (right, "right"),
                                    (left / "nested", "nested"))]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join(60)
            assert worker.exitcode == 0, worker.exitcode
        for p, label in ((left, "left"), (right, "right"),
                         (left / "nested", "nested")):
            verify_files(p, label)
        # A failed busy unmount must leave both views attached.
        fd = os.open(left / "marker", os.O_RDONLY)
        try:
            result = subprocess.run(["umount", str(left)], capture_output=True)
            assert result.returncode != 0
            assert os.read(fd, 16) == b"left"
            rejected(leftid, errno.EBUSY)
        finally:
            os.close(fd)
        unmount(left)
        (right / "after-first-unmount").write_bytes(b"still writable")
        # Roots keep their stat identity while any view retains the instance.
        mount(left, nestedid)
        assert left.stat().st_dev == nested_dev
        rejected(leftid, errno.EBUSY)
        rejected(5, errno.EBUSY)
        assert os.path.samefile(left / "..", base)
        verify_files(left, "nested")
        unmount(left)
        mount(left, nestedid, readonly=True)
        assert left.stat().st_dev == nested_dev
        expect_error(errno.EROFS, (left / "no-write").write_bytes, b"x")
        (right / "with-readonly-view").write_bytes(b"writable")
        unmount(left)
        mount(left, leftid)
        # Keep another view writing and committing through repeated detaches.
        (right / "during-unmount").mkdir()
        worker = multiprocessing.Process(
            target=writer, args=(right / "during-unmount", "during-unmount"))
        worker.start()
        for _ in range(5):
            unmount(left)
            mount(left, leftid)
        worker.join(60)
        assert worker.exitcode == 0, worker.exitcode
        verify_files(right / "during-unmount", "during-unmount")
        # Existing lock regression holds locks and interrupts blocked waiters
        # during forced reclaim. Keep a lock and VM object on the other view.
        fd = os.open(right / "marker", os.O_RDWR)
        mapfd = os.open(right / "file-0", os.O_RDONLY)
        mapping = mmap.mmap(mapfd, 12288, access=mmap.ACCESS_READ)
        os.close(mapfd)
        assert mapping[:] == payload("right", 0)
        fcntl.flock(fd, fcntl.LOCK_EX)
        try:
            subprocess.run([sys.executable, str(Path(__file__).with_name("locks.py")),
                            "reclaim", str(left)], check=True)
            mounted.remove(left)
            assert mapping[:] == payload("right", 0)
            assert os.pread(fd, 16, 0) == b"right"
            assert os.pwrite(fd, b"right", 0) == 5
            os.fsync(fd)
        finally:
            mapping.close()
            os.close(fd)
        mount(left, leftid)
        subprocess.run([sys.executable,
                        str(Path(__file__).with_name("kqueue.py")),
                        "revoke", str(left)], check=True)
        mounted.remove(left)
        verify_files(right, "right")
        mount(left, frozenid, readonly=True)
        assert (left / "marker").read_text() == "left/frozen"
        unmount(right)
        # The device stays open for writing until the final view detaches.
        mount(right, rightid)
        (right / "after-writer-unmount").write_bytes(b"writable again")
        unmount(left)
        unmount(right)
        mount(left, leftid, readonly=True)
        rejected(rightid, errno.EROFS)
        mount(right, rightid, readonly=True)
        expect_error(errno.EROFS, (right / "no-write").write_bytes, b"x")
        unmount(left)
        unmount(right)
        mount(left, 0)
        rejected(rightid, errno.EBUSY)
        rejected(nestedid, errno.EBUSY)
        verify(left)
        unmount(left)
        # Race two conflicting attaches; exactly one must succeed.
        commands = [["mount_btrfs", "-s", str(treeid), device, str(p)]
                    for p, treeid in ((left, leftid), (right, nestedid))]
        processes = [subprocess.Popen(command, stderr=subprocess.PIPE)
                     for command in commands]
        results = [process.communicate() for process in processes]
        successes = [p for p, process in zip((left, right), processes)
                     if process.returncode == 0]
        mounted.update(successes)
        assert len(successes) == 1, results
        unmount(successes[0])
    finally:
        for directory in list(mounted):
            subprocess.run(["umount", "-f", str(directory)], check=True)
    print("disjoint subvolume mount tests passed")


def verify(base):
    for name, label in (("left", "left"), ("right", "right"),
                        ("left/nested", "nested")):
        verify_files(base / name, label)
    for name in ("after-first-unmount", "with-readonly-view",
                 "after-writer-unmount"):
        assert (base / "right" / name).is_file()
    verify_files(base / "right/during-unmount", "during-unmount")
    assert (base / "left/frozen/marker").read_text() == "left/frozen"
    assert not (base / "left/frozen/new").exists()
    print("subvolume contents verified")


if __name__ == "__main__":
    if sys.argv[1] == "seed":
        seed(Path(sys.argv[2]))
    elif sys.argv[1] == "exercise":
        exercise(sys.argv[2], Path(sys.argv[3]).resolve(),
                 [int(value) for value in sys.argv[4:8]])
    elif sys.argv[1] == "verify":
        verify(Path(sys.argv[2]))
    else:
        raise SystemExit("use seed directory | exercise device mount-base "
                         "left-id nested-id frozen-id right-id | verify top")
