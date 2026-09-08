#!/usr/bin/env python3
"""Opaque xattrs: creation, inode change metadata, packed buckets, and remount."""
import errno
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import sys
import time

from namespace import expect_error
from send_receive import xattr
from subvolume import command


NAMES = ("metadata-file", "metadata-dir", "metadata-link")
VALUE = b"\0opaque\xff"
PARENT = {
    "user.test": VALUE,
    "system.posix_acl_default": struct.pack("<I", 2) + b"".join(
        struct.pack("<HHI", tag, perm, 0xffffffff)
        for tag, perm in ((1, 7), (4, 0), (32, 0))),
    "security.selinux": b"uninterpreted label\0",
    "btrfs.compression": b"zstd",
}
COLLISIONS = ("user.collision-3646043d825f0094b1055539",
              "user.collision-71a40ea0311399a610d2bf0e")


def stable(st):
    return st.st_ino, st.st_size, st.st_mode, st.st_mtime_ns, st.st_ctime_ns


def denied(code, *args, **kwargs):
    expect_error(code, lambda: xattr(*args, **kwargs))


def create(base):
    base.mkdir()
    (base / NAMES[0]).write_bytes(b"contents")
    (base / NAMES[1]).mkdir()
    os.symlink(NAMES[0], base / NAMES[2])
    # Make the xattr-only transaction newer than the inode's creation.
    os.sync()
    saved = {}
    for name in NAMES:
        path = base / name
        fd = queue = None
        if not path.is_symlink():
            fd = os.open(path, os.O_RDONLY)
            queue = select.kqueue()
            queue.control([select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                fflags=select.KQ_NOTE_ATTRIB)], 0, 0)
        try:
            for value in (b"first", b"replacement", None, VALUE):
                before = path.lstat()
                time.sleep(0.01)
                xattr(base, path, "user.test", value, remove=value is None)
                after = path.lstat()
                assert after.st_ctime_ns > before.st_ctime_ns, name
                assert stable(after)[:-1] == stable(before)[:-1], name
                if queue:
                    events = queue.control(None, 4, 2)
                    assert len(events) == 1, events
                    assert events[0].fflags == select.KQ_NOTE_ATTRIB, events
            before = path.lstat()
            denied(errno.ENOENT, base, path, "user.missing", remove=True)
            denied(errno.EINVAL, base, path, "user.large", bytes(65536))
            assert stable(path.lstat()) == stable(before), name
            assert xattr(base, path, "") == {"user.test": VALUE.hex()}
            assert stable(path.lstat()) == stable(before), name
            if queue:
                assert queue.control(None, 4, 0) == []
            saved[name] = stable(before)
        finally:
            if queue:
                queue.close()
            if fd is not None:
                os.close(fd)

    parent = base / "opaque"
    parent.mkdir()
    for name, value in PARENT.items():
        xattr(base, parent, name, value)
    (parent / "file").write_bytes(b"child")
    (parent / "dir").mkdir()
    os.symlink("file", parent / "link")
    os.mkfifo(parent / "fifo")
    subvol = str(parent.relative_to(base.parent) / "subvol")
    command(base.parent, "create", subvol)
    assert xattr(parent / "subvol", parent / "subvol", "") == {}
    (parent / "subvol/child").touch()
    command(base.parent, "delete", subvol)

    path = base / "collision"
    path.touch()
    for name, value in zip(COLLISIONS, (b"one", b"two")):
        xattr(base, path, name, value)
    xattr(base, path, COLLISIONS[0], VALUE)
    assert xattr(base, path, "") == {
        COLLISIONS[0]: VALUE.hex(), COLLISIONS[1]: b"two".hex()}
    xattr(base, path, COLLISIONS[0], remove=True)
    assert xattr(base, path, "") == {COLLISIONS[1]: b"two".hex()}
    xattr(base, path, COLLISIONS[1], remove=True)
    assert xattr(base, path, "") == {}

    # Grow a packed bucket up to its item limit, retaining both values and
    # inode metadata on failure. Removing a neighbor must recover capacity.
    xattr(base, path, COLLISIONS[1], bytes(1024))
    for length in range(1024, 65536, 1024):
        before = stable(path.lstat()), xattr(base, path, "")
        try:
            xattr(base, path, COLLISIONS[0], bytes(length))
        except OSError as error:
            assert error.errno == errno.ENOSPC, error
            assert (stable(path.lstat()), xattr(base, path, "")) == before
            break
    else:
        raise AssertionError("xattr collision bucket did not fill")
    xattr(base, path, COLLISIONS[1], remove=True)
    xattr(base, path, COLLISIONS[0], bytes(length))
    assert xattr(base, path, "") == {COLLISIONS[0]: bytes(length).hex()}
    xattr(base, path, COLLISIONS[0], remove=True)
    (base / "state.json").write_text(json.dumps(saved))
    os.sync()
    verify(base)


def verify(base):
    saved = json.loads((base / "state.json").read_text())
    for name in NAMES:
        path = base / name
        assert list(stable(path.lstat())) == saved[name], name
        assert xattr(base, path, "") == {"user.test": VALUE.hex()}, name
    parent = base / "opaque"
    assert xattr(base, parent, "") == {k: v.hex() for k, v in PARENT.items()}
    for path in parent.iterdir():
        assert xattr(base, path, "") == {}, path
    assert (parent / "file").read_bytes() == b"child"
    assert xattr(base, base / "collision", "") == {}


def readonly(base):
    verify(base)
    for name in NAMES:
        path = base / name
        before = path.lstat()
        denied(errno.EROFS, base, path, "user.test", b"no")
        denied(errno.EROFS, base, path, "user.test", remove=True)
        assert stable(path.lstat()) == stable(before), name


def disk(image):
    tree = subprocess.check_output(
        ["btrfs", "inspect-internal", "dump-tree", "-t", "fs", image], text=True)
    items, names = {}, {}
    for item in tree.split("\titem "):
        match = re.match(r"\d+ key \((\d+) INODE_ITEM 0\)", item)
        if match:
            items[int(match[1])] = item
        match = re.match(r"\d+ key \((\d+) INODE_REF \d+\)", item)
        if match:
            name = re.search(r"\bname: ([^\n]+)", item)[1]
            if name in NAMES:
                names[name] = int(match[1])
    assert set(names) == set(NAMES), names
    for name, ino in names.items():
        item = items[ino]
        generation, transid = map(int, re.search(
            r"\bgeneration (\d+) transid (\d+)", item).groups())
        assert transid > generation, (name, generation, transid)
        # Native inodes start at sequence 1; four successful xattr changes.
        assert int(re.search(r"\bsequence (\d+)", item)[1]) == 5, (name, item)
    print("xattr-only transactions advanced inode transid and sequence")


def malformed_seed(base):
    base.mkdir()
    path = base / "malformed"
    path.touch()
    for name in COLLISIONS:
        xattr(base, path, name, VALUE)
    os.sync()


def malformed_verify(base):
    assert xattr(base, base / "malformed", "") == {
        name: VALUE.hex() for name in COLLISIONS}


def malformed(base):
    path = base / "malformed"
    before = stable(path.lstat())
    # The damaged record follows the requested name in the same bucket.
    # Enumeration and both edit operations must validate that tail.
    denied(errno.EINVAL, base, path, "")
    denied(errno.EINVAL, base, path, COLLISIONS[0], b"replacement")
    denied(errno.EINVAL, base, path, COLLISIONS[0], remove=True)
    assert stable(path.lstat()) == before


def damage(image, journal, field):
    from chunks_fixture import checksum
    from mirrors import inspect
    from reclaim_chunks import chunks

    tree = inspect(str(image), "dump-tree", "-t", "fs")
    nodesize = int(re.search(r"^nodesize\s+(\d+)",
                            inspect(str(image), "dump-super"), re.M)[1])
    maps = chunks(image)
    saved = []
    fd = os.open(image, os.O_RDWR)
    try:
        for logical in map(int, re.findall(r"^leaf (\d+) items", tree, re.M)):
            chunk = next(c for c in maps if
                         c["logical"] <= logical < c["logical"] + c["length"])
            copies = [p + logical - chunk["logical"] for p in chunk["physical"]]
            block = bytearray(os.pread(fd, nodesize, copies[0]))
            assert all(os.pread(fd, nodesize, p) == block for p in copies)
            changed = False
            for slot in range(struct.unpack_from("<I", block, 96)[0]):
                _, kind, _, offset, size = struct.unpack_from(
                    "<QBQII", block, 101 + 25 * slot)
                if kind != 24:  # XATTR_ITEM
                    continue
                pos, end = 101 + offset, 101 + offset + size
                for name in COLLISIONS:
                    datalen, namelen = struct.unpack_from("<HH", block, pos + 25)
                    assert block[pos + 30:pos + 30 + namelen] == name.encode()
                    if name == COLLISIONS[1]:
                        if field == "length":
                            struct.pack_into("<H", block, pos + 25, 65535)
                        elif field == "name":
                            block[pos + 30] = 0
                        elif field == "type":
                            block[pos + 29] = 1
                        elif field == "location":
                            struct.pack_into("<Q", block, pos, 1)
                        else:
                            assert field == "hash"
                            block[pos + 30] ^= 1
                        changed = True
                    pos += 30 + namelen + datalen
                assert pos == end
            if changed:
                checksum(block)
                for address in copies:
                    saved.append((address, os.pread(fd, nodesize, address).hex()))
                    assert os.pwrite(fd, block, address) == nodesize
        assert saved
        journal.write_text(json.dumps(saved))
        os.fsync(fd)
    finally:
        os.close(fd)


def repair(image, journal):
    fd = os.open(image, os.O_RDWR)
    try:
        for address, data in json.loads(journal.read_text()):
            block = bytes.fromhex(data)
            assert os.pwrite(fd, block, address) == len(block)
        os.fsync(fd)
    finally:
        os.close(fd)


if __name__ == "__main__":
    phase = sys.argv[1]
    if phase == "damage":
        damage(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4])
    elif phase == "repair":
        repair(Path(sys.argv[2]), Path(sys.argv[3]))
    else:
        globals()[phase](Path(sys.argv[2]).resolve())
    print("xattrs", phase, "passed", flush=True)
