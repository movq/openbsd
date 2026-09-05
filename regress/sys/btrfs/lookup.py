#!/usr/bin/env python3
#
# Seed on the host, import with mkfs.btrfs --rootdir and --subvol rw:subvol,
# then verify in the VM.  This checks directory items from an independent
# writer, including packed hash collisions and subvolume locations.
#
import errno
import os
from pathlib import Path
import sys


NAMES = (
    "ordinary",
    "collision-3646043d825f0094b1055539",
    "collision-71a40ea0311399a610d2bf0e",
    "n" * 255,
    "utf8-\u00e9-\u96ea",
)


def contents(name, subvolume=False):
    prefix = "subvolume: " if subvolume else "top level: "
    return (prefix + name + "\n").encode("utf-8")


def seed(root):
    root.mkdir()
    for directory in (root, root / "subvol"):
        directory.mkdir(exist_ok=True)
        for name in NAMES:
            (directory / name).write_bytes(contents(name, directory != root))
        os.link(directory / "ordinary", directory / "alias")
        os.symlink(NAMES[1], directory / "symlink")
    os.symlink("subvol", root / "subvol-link")
    print("lookup fixture seeded", flush=True)


def verify(root):
    for directory in (root, root / "subvol", root / "subvol-link"):
        subvolume = directory != root
        for name in NAMES:
            assert (directory / name).read_bytes() == contents(name, subvolume)
        assert (directory / "alias").stat().st_ino == (
            directory / "ordinary").stat().st_ino
        assert (directory / "ordinary").stat().st_nlink == 2
        assert os.readlink(directory / "symlink") == NAMES[1]
        assert (directory / "symlink").read_bytes() == contents(
            NAMES[1], subvolume)
        expected = set(NAMES) | {"alias", "symlink"}
        if directory == root:
            expected |= {"subvol", "subvol-link"}
        assert set(os.listdir(directory)) == expected
        for name in ("missing", NAMES[1] + "-missing"):
            try:
                (directory / name).stat()
            except FileNotFoundError:
                pass
            else:
                raise AssertionError(("unexpected lookup success", name))
    assert os.stat(root / "subvol" / "..").st_ino == root.stat().st_ino
    assert (root / "subvol" / ".." / NAMES[2]).read_bytes() == contents(NAMES[2])
    try:
        os.mkdir(root / "subvol" / "forbidden")
    except OSError as error:
        assert error.errno == errno.EROFS, error
    else:
        raise AssertionError("mutation in an additional subvolume succeeded")
    print("imported hash and subvolume lookup verification passed", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("seed", "verify"):
        sys.exit(f"usage: {sys.argv[0]} seed|verify directory")
    globals()[sys.argv[1]](Path(sys.argv[2]).absolute())
