#!/usr/bin/env python3
"""Targeted send/receive fixture and stream rejection checks (OpenBSD/Linux)."""
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import socket
import signal
import stat
import struct
import subprocess
import sys
import time


CAPABILITY = struct.pack("<IIIII", 0x02000001, 1 << 10, 0, 0, 0)
DEFAULT_ACL = struct.pack("<I", 2) + b"".join(
    struct.pack("<HHI", tag, perm, uid)
    for tag, perm, uid in (
        (1, 7, 0xffffffff), (2, 7, 123), (4, 5, 0xffffffff),
        (16, 7, 0xffffffff), (32, 5, 0xffffffff)))


def xattr(root, path, name, value=None, remove=False):
    if not sys.platform.startswith("openbsd"):
        if remove:
            os.removexattr(path, name, follow_symlinks=False)
        elif value is None:
            return {key: os.getxattr(path, key, follow_symlinks=False).hex()
                    for key in os.listxattr(path, follow_symlinks=False)}
        else:
            os.setxattr(path, name, value, follow_symlinks=False)
        return
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    ctl = os.open("/dev/btrfs-control", os.O_RDWR)
    try:
        ino = path.lstat().st_ino
        buffer = ctypes.create_string_buffer(value if value is not None else 65536)
        cursor = 0
        result = {}
        while True:
            args = bytearray(struct.pack("=iIQQQ256s", fd,
                len(value) if value is not None else 0 if remove else 65536,
                ino, cursor, ctypes.addressof(buffer), name.encode()))
            request = 0x80000000 if value is not None or remove else 0xc0000000
            request |= len(args) << 16 | ord("B") << 8
            request |= 9 if remove else 8 if value is not None else 7
            try:
                fcntl.ioctl(ctl, request, args, True)
            except FileNotFoundError:
                if value is None and not remove:
                    return result
                raise
            if value is not None or remove:
                return
            _, size, _, cursor, _, attr = struct.unpack("=iIQQQ256s", args)
            result[attr.split(b"\0")[0].decode()] = buffer.raw[:size].hex()
    finally:
        os.close(ctl)
        os.close(fd)


def seed(root):
    root.mkdir(exist_ok=True)
    for name in ("a", "b", "gone", "kind", "empty", "acl-parent"):
        (root / name).mkdir()
    (root / "a/data").write_bytes(bytes(range(251)) * 1200)
    (root / "b/stable").write_bytes(b"unchanged" * 16000)
    os.link(root / "a/data", root / "alias")
    os.link(root / "b/stable", root / "b/second")
    (root / "gone/child").write_bytes(b"remove me")
    (root / "change-kind").write_bytes(b"file becomes a directory")
    with (root / "sparse").open("wb") as file:
        file.seek(500000)
        file.write(b"tail")
    os.symlink("a/data", root / "link")
    os.mkfifo(root / "fifo", 0o640)
    sock = socket.socket(socket.AF_UNIX)
    sock.bind(str(root / "socket"))
    sock.close()
    os.mknod(root / "device", stat.S_IFCHR | 0o600, os.makedev(1, 3))
    (root / "a/data").chmod(0o6751)
    os.chown(root / "b/stable", 123, 456)
    # Capabilities must be restored after data, ownership, and mode changes.
    for name in ("cap-keep", "cap-remove", "cap-change", "cap-stable"):
        path = root / name
        path.write_bytes(b"capability fixture\n")
        path.chmod(0o755)
        xattr(root, path, "security.capability", CAPABILITY)
    xattr(root, root / "a/data", "user.binary", b"\0one\xff")
    xattr(root, root / "a/data", "user.remove", b"old")
    xattr(root, root / "a", "user.directory", b"directory metadata")
    xattr(root, root / "acl-parent", "system.posix_acl_default", DEFAULT_ACL)
    xattr(root, root, "user.root", b"root metadata")
    for path in sorted(root.rglob("*"), reverse=True) + [root]:
        os.utime(path, ns=(1234567890123456789, 1234567800987654321),
                 follow_symlinks=False)


def mutate(root):
    # OpenBSD children have no inherited ACL. Linux receive must preserve that.
    (root / "acl-parent/new").write_bytes(b"new child")
    (root / "acl-parent/nested").mkdir()
    (root / "acl-parent/nested/child").write_bytes(b"nested child")
    (root / "a").rename(root / "temporary")
    (root / "b").rename(root / "a")
    (root / "temporary").rename(root / "b")
    with (root / "b/data").open("r+b") as file:
        file.seek(65536)
        file.write(b"changed" * 777)
        file.truncate(180123)
    (root / "alias").unlink()
    os.link(root / "b/data", root / "new-alias")
    (root / "a/second").unlink()
    (root / "gone/child").unlink()
    (root / "gone").rmdir()
    (root / "kind").rmdir()
    (root / "kind").write_bytes(b"directory becomes file")
    (root / "change-kind").unlink()
    (root / "change-kind").mkdir()
    (root / "change-kind/new").write_bytes(b"new child")
    (root / "link").unlink()
    os.symlink("b/data", root / "link")
    with (root / "sparse").open("r+b") as file:
        file.seek(20000)
        file.write(b"inside hole")
        file.truncate(800000)
    xattr(root, root / "b/data", "user.remove", remove=True)
    xattr(root, root / "b/data", "user.binary", b"\0two\xfe")
    xattr(root, root / "b", "user.directory", b"moved directory")
    xattr(root, root, "user.root", b"new root metadata")
    for name in ("cap-keep", "cap-remove", "cap-change"):
        path = root / name
        # Remove explicitly before Linux's write/chown can invalidate it.
        xattr(root, path, "security.capability", remove=True)
        path.write_bytes(b"changed capability fixture\n")
        os.chown(path, 123, 456)
        path.chmod(0o751)
        if name != "cap-remove":
            value = (CAPABILITY if name == "cap-keep" else
                     struct.pack("<IIIII", 0x02000001, 1 << 11, 0, 0, 0))
            xattr(root, path, "security.capability", value)
    for path in sorted(root.rglob("*"), reverse=True) + [root]:
        if path == root / "cap-stable":
            continue
        os.utime(path, ns=(1234567890123456789, 1334567800987654321),
                 follow_symlinks=False)


def clones(root):
    """Linux only: force both parent and same-root CLONE stream commands."""
    (root / "clones").mkdir()
    (root / "clones/new").write_bytes(b"new shared extents" * 8192)
    for source, target in ((root / "a/stable", root / "clones/parent"),
                           (root / "clones/new", root / "clones/within")):
        with source.open("rb") as src, target.open("wb") as dst:
            fcntl.ioctl(dst.fileno(), 0x40049409, src.fileno())


def manifest(root):
    result, links = {}, {}
    for path in [root, *sorted(root.rglob("*"))]:
        st = path.lstat()
        rel = str(path.relative_to(root))
        row = dict(mode=st.st_mode, uid=st.st_uid, gid=st.st_gid,
                   mtime=st.st_mtime_ns)
        if stat.S_ISREG(st.st_mode):
            row["size"] = st.st_size
            row["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        elif stat.S_ISLNK(st.st_mode):
            # Linux streams do not convey symlink permission bits.
            row["mode"] = stat.S_IFLNK
            row["target"] = os.readlink(path)
        elif stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode):
            row["device"] = [os.major(st.st_rdev), os.minor(st.st_rdev)]
        if not stat.S_ISDIR(st.st_mode):
            row["hardlink"] = links.setdefault((st.st_dev, st.st_ino), rel)
        row["xattrs"] = xattr(root, path, "")
        result[rel] = row
    return result


def readonly(root):
    try:
        (root / "readonly-probe").write_bytes(b"must not be written")
    except OSError as error:
        assert error.errno == 30, error
    else:
        (root / "readonly-probe").unlink()
        raise AssertionError(f"{root} lost its read-only flag")
    print(f"{root}: read-only state retained")


def compare(a, b):
    left, right = manifest(a), manifest(b)
    assert left == right, json.dumps(
        {p: [left.get(p), right.get(p)] for p in left.keys() | right.keys()
         if left.get(p) != right.get(p)}, indent=2)
    print("matching contents, links, modes, ownership, mtimes, and xattrs")


def crc32c(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc


def command(op, attrs=()):
    body = b"".join(struct.pack("<HH", kind, len(data)) + data
                    for kind, data in attrs)
    header = struct.pack("<IHI", len(body), op, 0)
    return struct.pack("<IHI", len(body), op, crc32c(header + body)) + body


def malformed(mount, destination):
    header = b"btrfs-stream\0" + struct.pack("<I", 1)
    uid = bytes(range(1, 17))
    end = command(21)
    for index, suffix in enumerate((
        b"",                                  # missing END
        command(11, [(15, b"../escape")]),
        command(11, [(15, b"/tmp/escape")]),
        command(3, [(15, b"bad\0name"), (3, struct.pack("<Q", 999))]),
        command(3, [(15, b"one"), (15, b"two"), (3, struct.pack("<Q", 999))]),
        command(65535),
        struct.pack("<IHI", 0xffffffff, 15, 0),
        command(3, [(15, b"badcrc"), (3, struct.pack("<Q", 999))])[:-1] + b"\xff",
        command(8, [(15, b"escape"), (3, struct.pack("<Q", 999)),
                    (17, b"/tmp")]) +
            command(3, [(15, b"escape/escaped"), (3, struct.pack("<Q", 1000))]),
    )):
        name = f"bad-{index}"
        begin = command(1, [(15, name.encode()), (1, uid),
                            (2, struct.pack("<Q", index + 1))])
        stream = header + begin + suffix + (end if suffix else b"")
        result = subprocess.run(["btrfs", "receive", str(mount), destination],
                                input=stream, capture_output=True)
        assert result.returncode != 0, (index, result.stderr)
        listing = subprocess.check_output(["btrfs", "subvolume", "list",
                                           str(mount)], text=True)
        assert f"ro path {destination}/{name}" not in listing
    # END alone and unsupported stream versions must never publish a subvolume.
    for stream in (header + end, b"btrfs-stream\0" + struct.pack("<I", 2) + end,
                   b"unterminated!" + struct.pack("<I", 1) + end):
        result = subprocess.run(["btrfs", "receive", str(mount), destination],
                                input=stream, capture_output=True)
        assert result.returncode != 0
    print("rejected malformed, truncated, unsupported, and escaping streams")


def lifecycle(mount, destination):
    """Run from an unrelated selected view; destinations are outside it."""
    header = b"btrfs-stream\0" + struct.pack("<I", 1)
    uid = bytes(range(17, 33))
    before = subprocess.check_output(["mount"])
    begin = command(1, [(15, b"interrupted"), (1, uid),
                        (2, struct.pack("<Q", 99))])
    process = subprocess.Popen(["btrfs", "receive", str(mount), destination],
                               stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    process.stdin.write(header + begin)
    process.stdin.flush()
    for _ in range(100):
        if b"/tmp/btrfs-stream." in subprocess.check_output(["mount"]):
            break
        time.sleep(0.05)
    else:
        process.kill()
        raise AssertionError("receive did not mount its disjoint destination")
    process.send_signal(signal.SIGTERM)
    _, errors = process.communicate(timeout=10)
    assert process.returncode != 0, errors
    assert subprocess.check_output(["mount"]) == before
    listing = subprocess.check_output(["btrfs", "subvolume", "list",
                                       str(mount)], text=True)
    assert f"rw path {destination}/interrupted" in listing
    print("interrupted receive remained writable and removed its temporary mount")


def send_lifecycle(mount, source):
    before = subprocess.check_output(["mount"])
    process = subprocess.Popen(["btrfs", "send", str(mount), source],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Leave stdout unread so a data-bearing send blocks after filling the pipe.
    for _ in range(100):
        if b"/tmp/btrfs-stream." in subprocess.check_output(["mount"]):
            break
        time.sleep(0.05)
    else:
        process.kill()
        raise AssertionError("send did not mount its disjoint source")
    time.sleep(0.1)
    process.send_signal(signal.SIGTERM)
    _, errors = process.communicate(timeout=10)
    assert process.returncode != 0, errors
    assert subprocess.check_output(["mount"]) == before
    print("interrupted blocked send removed its temporary mount")


def busy(mount, destination):
    header = b"btrfs-stream\0" + struct.pack("<I", 1)
    begin = command(1, [(15, b"busy"), (1, bytes(range(33, 49))),
                        (2, struct.pack("<Q", 100))])
    create = command(3, [(15, b"file"), (3, struct.pack("<Q", 999))])
    process = subprocess.Popen(["btrfs", "receive", str(mount), destination],
                               stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    process.stdin.write(header + begin + create)
    process.stdin.flush()
    path = mount / destination / "busy/file"
    for _ in range(100):
        if path.exists():
            break
        time.sleep(0.05)
    else:
        process.kill()
        raise AssertionError("receive did not create its file")
    with path.open("r+b"):
        _, errors = process.communicate(command(21), timeout=10)
        assert process.returncode != 0, errors
    listing = subprocess.check_output(["btrfs", "subvolume", "list",
                                       str(mount)], text=True)
    assert f"rw path {destination}/busy" in listing
    print("active writer prevented receive finalization")


if __name__ == "__main__":
    action, *args = sys.argv[1:]
    if action == "manifest":
        print(json.dumps(manifest(Path(args[0])), indent=2, sort_keys=True))
    elif action in ("malformed", "lifecycle", "send_lifecycle", "busy"):
        globals()[action](Path(args[0]), args[1])
    else:
        globals()[action](*(Path(arg) for arg in args))
