#!/usr/bin/env python3
"""Destructive targeted balance test using scratch1 in both VMs.

Reformats OpenBSD sd1c / Linux sdb. Never mounts in both guests concurrently.
The second scratch disk is used only with --devices. Failures retain state.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime
import json
from pathlib import Path
import re
import subprocess
import time


def run(args):
    args.results.mkdir(parents=True, exist_ok=False)
    log = (args.results / "commands.log").open("w", buffering=1)
    point = "/mnt/btrfs-balance"

    def remote(vm, command, fail=False, code=None):
        log.write(f"{vm}: {command}\n")
        result = subprocess.run(
            ["timeout", str(args.timeout), "ssh", "-o", "ConnectTimeout=10",
             vm, command], input=code, text=True, capture_output=True)
        log.write(result.stdout + result.stderr)
        assert result.returncode not in (124, 255), (
            command, result.returncode, result.stderr)
        assert bool(result.returncode) == fail, (
            command, result.returncode, result.stdout, result.stderr)
        return result.stdout

    def b(command, **kwargs):
        return remote(args.openbsd, command, **kwargs)

    def l(command, **kwargs):
        return remote(args.linux, command, **kwargs)

    def mount(ro=False):
        b(f"mount_btrfs {'-o ro' if ro else ''} /dev/sd1c {point}")

    def layout(name):
        l("blockdev --flushbufs /dev/sdb")
        if args.devices:
            l("blockdev --flushbufs /dev/sdc")
        tree = "11" if args.block_groups and not args.enospc else "extent"
        text = l(f"btrfs inspect-internal dump-tree -t {tree} /dev/sdb")
        (args.results / f"{name}-allocation.txt").write_text(text)
        groups = {}
        for part in text.split("\titem ")[1:]:
            match = re.search(r"key \((\d+) BLOCK_GROUP_ITEM (\d+)\)", part)
            if match:
                used = re.search(r"block group used (\d+).*flags (\S+)", part)
                groups[int(match[1])] = dict(
                    length=int(match[2]), used=int(used[1]), type=used[2])
        assert groups, text
        (args.results / f"{name}.json").write_text(
            json.dumps(groups, indent=2))
        return groups

    def verify(name):
        l("blockdev --flushbufs /dev/sdb")
        if args.devices:
            l("blockdev --flushbufs /dev/sdc && btrfs device scan /dev/sdb /dev/sdc")
        l("btrfs check --readonly --check-data-csum /dev/sdb")
        l(f"mount -o ro /dev/sdb {point}")
        l("sha256sum -c /root/btrfs-balance.sha")
        l("python3 -", code=f"""
import os
root = {point!r}
assert os.stat(root + '/base/random').st_ino == os.stat(root + '/base/link').st_ino
assert os.getxattr(root + '/base/random', 'user.balance') == b'preserved'
assert len(os.listdir(root + '/base/names')) == 3000
""")
        l(f"umount {point}")
        return layout(name)

    for vm in (args.openbsd, args.linux):
        assert "type btrfs" not in remote(vm, "mount")
        remote(vm, f"mkdir -p {point}")
    if args.enospc:
        l("mkfs.btrfs -f -K -b 134217728 /dev/sdb")
        l(f"mount /dev/sdb {point}")
        l(f"dd if=/dev/urandom of={point}/full bs=1M count=120 status=none",
          fail=True)
        l(f"sync && sha256sum {point}/full > /root/btrfs-balance-full.sha && "
          f"umount {point}")
        before = layout("full-before")
        mount()
        for option in ("-musage=100", "-dusage=100"):
            output = b(f"btrfs balance start {option} {point}", fail=True)
            assert "relocate 0 out of" in output
            status = b(f"btrfs balance status {point}")
            assert "No space left on device" in status
            b(f"mkdir {point}/writable && rmdir {point}/writable")
        b(f"umount {point}")
        after = layout("full-after")
        assert set(before) == set(after), (before, after)
        l("btrfs check --readonly --check-data-csum /dev/sdb")
        l(f"mount -o ro /dev/sdb {point} && "
          f"sha256sum -c /root/btrfs-balance-full.sha && umount {point}")
        print(f"balance ENOSPC test passed: {args.results}")
        return
    devices = "/dev/sdb /dev/sdc" if args.devices else "/dev/sdb"
    features = []
    if args.block_groups:
        features.append("block-group-tree")
    if args.no_free_space_tree:
        features.append("^free-space-tree")
    feature_option = "-O " + ",".join(features) if features else ""
    l(f"mkfs.btrfs -f -K -b 2147483648 -d {args.data} -m {args.metadata} "
      f"-n {args.nodesize} --csum {args.csum} {feature_option} {devices}")
    cache = ",nospace_cache" if args.no_free_space_tree else ""
    l(f"mount -o compress=zstd{cache} /dev/sdb {point}")
    l("python3 -", code=f"""
from pathlib import Path
import fcntl, os, struct
root = Path({point!r})
(root / 'base/names').mkdir(parents=True)
(root / 'base/random').write_bytes(os.urandom(16 * 1024 * 1024))
(root / 'base/compressed').write_bytes(b'compressed balance data\\n' * 131072)
for source, name, offset, length in (
        ('random', 'partial', 1024 * 1024, 3 * 1024 * 1024),
        ('compressed', 'partial-compressed', 4096, 128 * 1024)):
    src = os.open(root / 'base' / source, os.O_RDONLY)
    dst = os.open(root / name, os.O_CREAT | os.O_RDWR, 0o600)
    os.fsync(src)
    fcntl.ioctl(dst, 0x4020940d, struct.pack('qQQQ', src, offset, length, 0))
    os.close(src)
    os.close(dst)
os.link(root / 'base/random', root / 'base/link')
os.setxattr(root / 'base/random', 'user.balance', b'preserved')
for i in range(3000):
    (root / 'base/names' / (str(i) + '-' + 'x' * 64)).touch()
""")
    l(f"cp --reflink=always {point}/base/random {point}/clone && "
      f"fallocate -l 4M {point}/prealloc && "
      f"dd if=/dev/urandom of={point}/prealloc bs=4096 count=3 seek=9 "
      f"conv=notrunc status=none && mkdir {point}/nocow && "
      f"chattr +C {point}/nocow && "
      f"dd if=/dev/urandom of={point}/nocow/data bs=1M count=1 status=none && "
      f"btrfs subvolume snapshot -r {point} {point}/snap && "
      f"find {point} -type f -size +0c -exec sha256sum {{}} \\; "
      f"> /root/btrfs-balance.sha && umount {point}")
    initial = layout("initial")
    if args.devices:
        # The driver requires explicit additional members.
        def mount(ro=False):
            b(f"mount_btrfs {'-o ro' if ro else ''} -d /dev/sd2c "
              f"/dev/sd1c {point}")
    mount(ro=True)
    b(f"btrfs balance start -dusage=0 {point}", fail=True)
    b(f"umount {point}")
    mount()
    if args.crash:
        from run_all import Runner, Serial
        crash_args = argparse.Namespace(
            vm=args.openbsd, image="/home/mike/obj/scratch1.img",
            device="/dev/sd1c", mountpoint=point, vm_source="/mnt/src",
            monitor=args.monitor, timeout=args.timeout, boot_timeout=180)
        harness = Runner(crash_args, args.results)
        harness.log = log
        serial_log = (args.results / "serial.log").open("w", buffering=1)
        harness.serial = Serial(args.serial, serial_log)
        cases = [
            ("data-before-super", "-dusage=100",
             ["btrfs_relocate_data", "btrfs_write_super_mirrors"], 1),
            ("data-between-supers", "-dusage=100",
             ["btrfs_relocate_data", "btrfs_write_super_mirrors", "bwrite"], 2),
            ("metadata-before-super", "-musage=100",
             ["btrfs_balance_relocate", "btrfs_write_super_mirrors"], 1),
        ]
        try:
            for name, option, targets, hits in cases:
                harness.crash_break(
                    ["btrfs", "balance", "start", option, point],
                    targets, hits=hits)
                harness.boot()
                # Heal stale super mirrors before handing the image to Linux.
                mount()
                b(f"echo recovered > {point}/recovery && "
                  f"rm {point}/recovery && umount {point}")
                verify(name)
                mount()
            b(f"btrfs balance start {point} && umount {point}")
            verify("crash-restarted")
        finally:
            harness.serial.close()
            serial_log.close()
            if harness.monitor is not None:
                harness.monitor.close()
        print(f"balance crash tests passed: {args.results}")
        return
    for option in ("-dusage=101", "-musage=80..20", "-dconvert=dup",
                   "-dusage=garbage", "-dusage=1,usage=2", "-dlimit=-1"):
        b(f"btrfs balance start {option} {point}", fail=True)
    result = b(f"btrfs balance start -dlimit=0 {point}")
    assert "relocate 0 out of 0" in result
    result = b(f"btrfs balance start -dusage=0 {point}")
    empty = {logical for logical, group in initial.items()
             if group["type"].startswith("DATA") and group["used"] == 0}
    assert f"relocate {len(empty)} out of {len(empty)}" in result
    b(f"umount {point}")
    after_empty = layout("empty")
    assert not empty.intersection(after_empty)
    assert all(logical in after_empty for logical in initial if logical not in empty)

    mount()
    result = b(f"btrfs balance start -dusage=1..100,limit=1 {point}")
    assert "relocate 1 out of 1" in result, result
    b(f"umount {point}")
    after_limit = verify("limit")
    removed = set(after_empty) - set(after_limit)
    expected = max(logical for logical, group in after_empty.items()
                   if group["type"].startswith("DATA") and
                   group["used"] * 100 >= group["length"])
    assert removed == {expected}, (removed, expected)

    mount()
    # Shared snapshot leaves, inode/xattr records, all global trees, bootstrap
    # system mappings, and metadata reservations must survive evacuation.
    ceiling = max(10, max(g["used"] * 100 // g["length"] + 1
                         for g in after_limit.values()
                         if g["type"].startswith(("METADATA", "SYSTEM"))))
    result = b(f"btrfs balance start -musage={ceiling} {point}")
    assert "Done" in result
    b(f"umount {point}")
    after_meta = verify("metadata")
    for logical, group in after_limit.items():
        if group["type"].startswith(("METADATA", "SYSTEM")):
            assert logical not in after_meta, (logical, group)
        else:
            assert after_meta[logical] == group, (logical, group)
    old_metadata = sum(g["length"] for g in after_limit.values()
                       if g["type"].startswith("METADATA"))
    if old_metadata > 32 * 1024 * 1024:
        assert sum(g["length"] for g in after_meta.values()
                   if g["type"].startswith("METADATA")) < old_metadata

    mount()
    # Open descriptors before starting: reads and writes must wait safely
    # across relocation. Status/cancel also need to work via a new pathname.
    b("rm -f /tmp/balance-ready /tmp/balance-go")
    digest = b(f"sha256 -q {point}/base/random").strip()
    with ThreadPoolExecutor(2) as pool:
        worker = pool.submit(b, "python3 -", code=f"""
import hashlib, os, time
from pathlib import Path
src = os.open({point!r} + '/base/random', os.O_RDONLY)
dst = os.open({point!r} + '/concurrent', os.O_RDWR | os.O_CREAT, 0o600)
Path('/tmp/balance-ready').touch()
while not Path('/tmp/balance-go').exists():
    time.sleep(0.02)
data = os.read(src, 16 * 1024 * 1024)
assert hashlib.sha256(data).hexdigest() == {digest!r}
assert os.write(dst, b'concurrent balance write' * 4096) == 24 * 4096
os.fsync(dst)
os.close(src)
os.close(dst)
""")
        for attempt in range(100):
            if b("test ! -f /tmp/balance-ready || echo ready").strip():
                break
            time.sleep(0.02)
        else:
            raise AssertionError("worker did not open its descriptors")
        b("python3 -", code=f"""
from pathlib import Path
import re, subprocess
point = {point!r}
balance = subprocess.Popen(['btrfs', 'balance', 'start', '-musage=100', point])
try:
    for attempt in range(1000):
        status = subprocess.check_output(
            ['btrfs', 'balance', 'status', point], text=True)
        if ('Balance running' in status and
                re.search(r'about [1-9][0-9]* chunks', status)):
            print(status, flush=True)
            break
        assert balance.poll() is None, 'balance finished before status'
    else:
        raise AssertionError('balance never started')
    assert subprocess.run(
        ['btrfs', 'balance', 'start', '-dlimit=0', point]).returncode != 0
    Path('/tmp/balance-go').touch()
    creator = subprocess.Popen(['touch', point + '/blocked'])
    subprocess.run(['btrfs', 'balance', 'cancel', point], check=True)
    assert balance.wait(timeout=60) != 0, 'canceled balance succeeded'
    assert creator.wait(timeout=60) == 0
    Path(point + '/blocked').unlink()
finally:
    Path('/tmp/balance-go').touch()
    if balance.poll() is None:
        balance.terminate()
        balance.wait(timeout=60)
""")
        worker.result()
    b("python3 -", code=f"""
from pathlib import Path
path = Path({point!r}) / 'concurrent'
assert path.read_bytes() == b'concurrent balance write' * 4096
path.unlink()
""")
    b(f"umount {point}")
    verify("canceled")
    mount()
    b(f"btrfs balance start -dusage=100 {point}")
    b(f"btrfs balance status {point}")
    b(f"btrfs balance cancel {point}", fail=True)
    b(f"dd if=/dev/zero of={point}/after bs=4096 count=1")
    b(f"rm {point}/after && umount {point}")
    verify("final")

    # Removing the last empty data group must not prevent later allocation,
    # including after remount when no data chunk supplies a growth template.
    l(f"mkfs.btrfs -f -K -b 2147483648 -d {args.data} -m {args.metadata} "
      f"-n {args.nodesize} --csum {args.csum} {feature_option} {devices}")
    for remount in (False, True):
        mount()
        b(f"btrfs balance start -dusage=0 {point}")
        if remount:
            b(f"umount {point}")
            empty = layout("no-data-groups")
            assert not any(g["type"].startswith("DATA") for g in empty.values())
            mount()
        b(f"dd if=/dev/zero of={point}/regrown bs=4096 count=4 && "
          f"umount {point}")
        l("blockdev --flushbufs /dev/sdb")
        if args.devices:
            l("blockdev --flushbufs /dev/sdc && btrfs device scan /dev/sdb /dev/sdc")
        l("btrfs check --readonly --check-data-csum /dev/sdb")
        l(f"mount -o ro /dev/sdb {point}")
        l("python3 -", code=f"""
from pathlib import Path
assert (Path({point!r}) / 'regrown').read_bytes() == bytes(16384)
""")
        l(f"umount {point}")
        mount()
        b(f"rm {point}/regrown && umount {point}")
    print(f"balance passed: {args.results}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openbsd", default="root@10.77.0.2")
    parser.add_argument("--linux", default="root@10.77.0.3")
    parser.add_argument("--data", choices=("single", "dup"), default="single")
    parser.add_argument("--metadata", choices=("single", "dup"), default="dup")
    parser.add_argument("--nodesize", type=int, choices=(4096, 16384), default=16384)
    parser.add_argument("--csum", choices=("crc32c", "xxhash"), default="crc32c")
    parser.add_argument("--devices", action="store_true")
    parser.add_argument("--crash", action="store_true")
    parser.add_argument("--enospc", action="store_true")
    parser.add_argument("--block-groups", action="store_true")
    parser.add_argument("--no-free-space-tree", action="store_true")
    parser.add_argument("--serial", default="/tmp/serial.sock")
    parser.add_argument("--monitor", default="/tmp/monitor.sock")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--results", type=Path, default=Path("/tmp") /
                        ("btrfs-balance-" + datetime.datetime.now().strftime(
                            "%Y%m%d-%H%M%S")))
    options = parser.parse_args()
    if options.enospc and (options.devices or options.crash):
        parser.error("--enospc is a separate single-device test")
    if options.block_groups and options.no_free_space_tree:
        parser.error("Linux mkfs requires a free-space tree with block groups")
    run(options)
