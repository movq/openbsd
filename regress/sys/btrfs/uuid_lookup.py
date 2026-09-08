#!/usr/bin/env python3
"""UUID lookup, identity selection, and unmounted index fallback fixtures."""
import errno
import fcntl
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import uuid as uuidlib

from subvolume import command


IDENTITY = struct.Struct("=iIQQQQ16s16s1024s1024s")
FIELDS = ("fd", "flags", "id", "ctransid", "stransid", "fd_treeid",
          "uuid", "received_uuid", "path", "access")


def request(mount, operation, **inputs):
    fd = os.open(mount, os.O_RDONLY | os.O_DIRECTORY)
    ctl = os.open("/dev/btrfs-control",
                  os.O_RDWR if operation == 6 else os.O_RDONLY)
    try:
        values = dict.fromkeys(FIELDS[:6], 0)
        values.update(dict.fromkeys(FIELDS[6:], b""))
        values.update(inputs, fd=fd)
        data = bytearray(IDENTITY.pack(*(values[key] for key in FIELDS)))
        code = (0x80000000 if operation == 6 else 0xc0000000)
        code |= len(data) << 16 | ord("B") << 8 | operation
        fcntl.ioctl(ctl, code, data, True)
        return dict(zip(FIELDS, IDENTITY.unpack(data)))
    finally:
        os.close(ctl)
        os.close(fd)


def info(mount, path):
    return request(mount, 5, path=path.encode())


def lookup(mount, uuid, transid, expected=None, error=None, **inputs):
    try:
        result = request(mount, 12, uuid=uuid, stransid=transid, **inputs)
    except OSError as exc:
        assert exc.errno == error, (exc, error)
        return
    assert error is None, (result, error)
    assert result["id"] == expected["id"], (result, expected)
    for key in FIELDS[1:]:
        assert result[key] == expected[key], (key, result, expected)


def receive_identity(mount, name, uuid, transid):
    command(mount, "create", name)
    root = info(mount, name)
    request(mount, 6, id=root["id"], received_uuid=uuid, stransid=transid)
    return info(mount, name)


def create(mount):
    command(mount, "create", "source")
    command(mount, "snapshot", "source", "native", readonly=True)
    native = info(mount, "native")
    uuid, transid = native["uuid"], native["ctransid"]
    lookup(mount, uuid, transid, native)
    writable = info(mount, "source")
    lookup(mount, writable["uuid"], writable["ctransid"], error=errno.ENOENT)
    lookup(mount, uuid, transid + 100, error=errno.ENOENT)
    lookup(mount, bytes(16), transid, error=errno.ENOENT)
    lookup(mount, bytes(range(16)), transid, error=errno.ENOENT)
    for reserved in (dict(flags=1), dict(id=native["id"]),
                     dict(path=b"native"), dict(ctransid=1),
                     dict(fd_treeid=5), dict(received_uuid=uuid),
                     dict(access=b"/")):
        lookup(mount, uuid, transid, error=errno.EINVAL, **reserved)
    # One UUID indexes several IDs; transaction ID filters the candidates.
    receive_identity(mount, "wrong-transid", uuid, transid + 100)
    preferred = receive_identity(mount, "received", uuid, transid)
    lookup(mount, uuid, transid, preferred)
    receive_identity(mount, "duplicate", uuid, transid)
    lookup(mount, uuid, transid, error=errno.EEXIST)
    command(mount, "delete", "duplicate")
    lookup(mount, uuid, transid, preferred)
    command(mount, "delete", "received")
    lookup(mount, uuid, transid, native)
    receive_identity(mount, "received", uuid, transid)
    command(mount, "snapshot", "source", "deleted", readonly=True)
    deleted = info(mount, "deleted")
    command(mount, "delete", "deleted")
    lookup(mount, deleted["uuid"], deleted["ctransid"], error=errno.ENOENT)
    verify(mount)


def verify(mount):
    native = info(mount, "native")
    received = info(mount, "received")
    wrong = info(mount, "wrong-transid")
    lookup(mount, native["uuid"], native["ctransid"], received)
    lookup(mount, native["uuid"], wrong["stransid"], wrong)
    lookup(mount, received["uuid"], received["ctransid"], received)
    source = info(mount, "source")
    lookup(mount, source["uuid"], source["ctransid"], error=errno.ENOENT)
    lookup(mount, native["uuid"], native["ctransid"] + 200, error=errno.ENOENT)
    print("UUID lookup identities, transaction IDs and read-only filtering passed")


def missing(mount):
    """An intentionally hidden current index must not silently trigger a scan."""
    native = info(mount, "native")
    lookup(mount, native["uuid"], native["ctransid"], error=errno.ENOENT)
    received = info(mount, "received")
    lookup(mount, received["uuid"], received["ctransid"], error=errno.ENOENT)
    print("current UUID index used for negative lookups")


def streams(mount):
    """Exercise both receive callers with native and received clone sources."""
    from send_receive import command as stream_command

    command(mount, "create", "lookup-source")
    payload = bytes(range(256)) * 512
    (mount / "lookup-source/data").write_bytes(payload)
    command(mount, "snapshot", "lookup-source", "lookup-base", readonly=True)
    base = info(mount, "lookup-base")
    destination = mount / "lookup-received"
    destination.mkdir()
    for name in ("external-native", "external-received"):
        stream = b"btrfs-stream\0" + struct.pack("<I", 1)
        stream += stream_command(2, [
            (15, name.encode()), (1, uuidlib.uuid4().bytes),
            (2, struct.pack("<Q", 100)),
            (20, base["uuid"]), (21, struct.pack("<Q", base["ctransid"]))])
        stream += stream_command(3, [(15, b"clone"),
                                     (3, struct.pack("<Q", 9999))])
        stream += stream_command(16, [
            (15, b"clone"), (18, struct.pack("<Q", 0)),
            (24, struct.pack("<Q", len(payload))),
            (20, base["uuid"]), (21, struct.pack("<Q", base["ctransid"])),
            (22, b"data"), (23, struct.pack("<Q", 0))])
        stream += stream_command(21)
        subprocess.run(["btrfs", "receive", str(mount), "lookup-received"],
                       input=stream, check=True)
        assert (destination / name / "clone").read_bytes() == payload
        assert info(mount, "lookup-received/" + name)["flags"] == 1
        if name == "external-native":
            stream = subprocess.check_output(
                ["btrfs", "send", str(mount), "lookup-base"])
            subprocess.run(["btrfs", "receive", str(mount), "lookup-received"],
                           input=stream, check=True)
            received = info(mount, "lookup-received/lookup-base")
            lookup(mount, base["uuid"], base["ctransid"], received)
    print("receive snapshot-parent and external clone UUID lookups passed")


def disk(image, mode, journal):
    """Edit only unmounted disposable images; restore before independent checks."""
    from chunks_fixture import checksum
    from mirrors import inspect, SUPERS
    from reclaim_chunks import chunks

    if mode == "restore":
        with image.open("r+b", buffering=0) as stream:
            for offset, data in json.loads(journal.read_text()):
                os.pwrite(stream.fileno(), bytes.fromhex(data), offset)
            os.fsync(stream.fileno())
        return
    text = inspect(str(image), "dump-super")
    nodesize = int(re.search(r"^nodesize\s+(\d+)", text, re.M)[1])
    csum = int(re.search(r"^csum_type\s+(\d+)", text, re.M)[1])
    maps = chunks(image)
    saved = []
    with image.open("r+b", buffering=0) as stream:
        fd = stream.fileno()

        def write(offset, block):
            saved.append((offset, os.pread(fd, len(block), offset).hex()))
            # Save originals before changing any byte on disk.
            journal.write_text(json.dumps(saved))
            checksum(block, csum)
            assert os.pwrite(fd, block, offset) == len(block)

        if mode == "stale":
            for offset in SUPERS:
                block = bytearray(os.pread(fd, 4096, offset))
                if len(block) != 4096 or block[64:72] != b"_BHRfS_M":
                    continue
                # uuid_tree_generation follows cache_generation.
                struct.pack_into("<Q", block, 0x233, 0)
                write(offset, block)
        else:
            assert mode in ("absent", "hide")
            tree = "root" if mode == "absent" else "uuid"
            text = inspect(str(image), "dump-tree", "-t", tree)
            edits = 0
            for logical in map(int, re.findall(r"^leaf (\d+) items", text, re.M)):
                chunk = next(c for c in maps if
                             c["logical"] <= logical <
                             c["logical"] + c["length"])
                copies = [p + logical - chunk["logical"]
                          for p in chunk["physical"]]
                block = bytearray(os.pread(fd, nodesize, copies[0]))
                changed = False
                for i in range(struct.unpack_from("<I", block, 96)[0]):
                    pos = 101 + 25 * i
                    owner, kind, _, start, size = struct.unpack_from(
                        "<QBQII", block, pos)
                    if mode == "absent" and owner == 9 and kind == 132:
                        block[pos + 8] = 0
                        changed = True
                    elif mode == "hide" and kind in (251, 252):
                        assert size and size % 8 == 0
                        for index in range(size // 8):
                            struct.pack_into("<Q", block,
                                             101 + start + index * 8,
                                             0xffffffffffffff00)
                        changed = True
                if changed:
                    for offset in copies:
                        write(offset, block)
                    edits += 1
            assert edits
        os.fsync(fd)


if __name__ == "__main__":
    action, *arguments = sys.argv[1:]
    if action == "disk":
        disk(Path(arguments[0]), arguments[1], Path(arguments[2]))
    else:
        globals()[action](*(Path(arg) for arg in arguments))
