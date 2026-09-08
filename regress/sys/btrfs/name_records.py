#!/usr/bin/env python3
"""One full directory hash bucket shared by inode and subvolume operations."""
import errno
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys

from namespace import COLLISIONS, expect_error
from subvolume import command, listing
from unlink import snapshot


def create(base):
    base.mkdir()
    bucket = base / "bucket"
    bucket.mkdir()
    source = base / "source"
    source.write_bytes(b"source")
    mount = base.parent
    command(mount, "create", str(base.relative_to(mount) / "subvol"))

    # Equal-length CRC collisions can be substituted in any concatenation.
    # Seven segments give 128 names of 238 bytes, enough to fill 16K nodes.
    names = ["".join(parts) for parts in itertools.product(COLLISIONS, repeat=7)]
    count = 0
    for name in names:
        before = snapshot(bucket)
        try:
            (bucket / name).touch(exist_ok=False)
        except OSError as error:
            assert error.errno == errno.ENOSPC, error
            assert snapshot(bucket) == before
            break
        count += 1
    else:
        raise AssertionError("collision bucket did not fill")
    assert count >= 2
    absent = bucket / names[count]
    before = snapshot(bucket), snapshot(source), listing(mount)
    for function, args in ((os.link, (source, absent)),
                           (os.rename, (source, absent)),
                           (os.mkdir, (absent,)),
                           (os.symlink, ("missing", absent))):
        expect_error(errno.ENOSPC, function, *args)
        assert not absent.exists()
        assert (snapshot(bucket), snapshot(source), listing(mount)) == before
    for operation, paths in (
            ("create", [str(absent.relative_to(mount))]),
            ("snapshot", [str((base / "subvol").relative_to(mount)),
                          str(absent.relative_to(mount))])):
        result = subprocess.run(["btrfs", "subvolume", operation, str(mount),
                                 *paths], capture_output=True, text=True)
        assert result.returncode != 0 and "No space left" in result.stderr, result
        assert not absent.exists()
        assert (snapshot(bucket), snapshot(source), listing(mount)) == before

    # Removing and adding to the same key must use the resulting item size.
    os.rename(bucket / names[0], absent)
    os.rename(bucket / names[1], absent)
    os.link(source, bucket / names[0])
    (bucket / names[0]).unlink()
    command(mount, "create", str((bucket / names[0]).relative_to(mount)))
    command(mount, "delete", str((bucket / names[0]).relative_to(mount)))
    command(mount, "snapshot", str((base / "subvol").relative_to(mount)),
            str((bucket / names[0]).relative_to(mount)))
    command(mount, "delete", str((bucket / names[0]).relative_to(mount)))
    os.rename(source, bucket / names[0])
    command(mount, "delete", str((base / "subvol").relative_to(mount)))
    (base / "state.json").write_text(json.dumps({
        "names": [names[0], *names[2:count], names[count]],
        "source": names[0],
    }))
    os.sync()
    verify(base)
    print(f"shared directory bucket capacity passed with {count} records")


def verify(base):
    state = json.loads((base / "state.json").read_text())
    bucket = base / "bucket"
    assert sorted(os.listdir(bucket)) == sorted(state["names"])
    assert (bucket / state["source"]).read_bytes() == b"source"
    assert bucket.stat().st_size == sum(len(name) * 2 for name in state["names"])
    for name in state["names"]:
        assert (bucket / name).stat().st_nlink == 1
    assert not (base / "source").exists() and not (base / "subvol").exists()


if __name__ == "__main__":
    phase, path = sys.argv[1:]
    globals()[phase](Path(path).resolve())
    print("name_records", phase, "passed", flush=True)
