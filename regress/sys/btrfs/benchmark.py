#!/usr/bin/env python3
"""OpenBSD/Linux workload runner for a freshly formatted, unmounted scratch FS.

Creates and removes "tree" and "large" beneath the mountpoint. Formatting is
deliberately external. ZFS expects an exported scratch pool and a dataset
whose mountpoint is already configured. Run under an external timeout, since
a kernel hang can also block the Python process.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import tarfile
import time


p = argparse.ArgumentParser()
p.add_argument("name")
p.add_argument("device")
p.add_argument("mountpoint")
p.add_argument("--type", choices=["ffs", "btrfs", "zfs"], required=True)
p.add_argument("--options", default="noatime",
               help="FFS/Btrfs mount options; configure ZFS properties externally")
p.add_argument("--zpool-device", help="ZFS import search device; required for ZFS")
p.add_argument("--archive", help="source archive; required unless --io-only")
p.add_argument("--out", required=True)
workloads = p.add_mutually_exclusive_group()
workloads.add_argument("--delete-only", action="store_true",
                       help="extract and delete the archive, skipping other workloads")
workloads.add_argument("--io-only", action="store_true",
                       help="run only sequential writes and reads")
p.add_argument("--io-mib", type=int, default=2048,
               help="size of each sequential I/O file in MiB (default: 2048)")
p.add_argument("--io-iterations", type=int, default=2)
p.add_argument("--io-pattern", choices=["zero", "pattern"], default="zero",
               help="pattern uses a deterministic nonzero 1 MiB buffer")
p.add_argument("--read-repeat", action="store_true",
               help="also time an immediate sequential read repeat")
p.add_argument("--verify-data", action="store_true",
               help="compare extracted files with the archive and I/O files with their pattern")
p.add_argument("--checkpoints", action="store_true",
               help="emit unmounted checkpoints and wait for 'continue' on stdin")
p.add_argument("--observe", action="store_true",
               help="pause before/after measurements for external counter collection")
a = p.parse_args()
if not a.io_only and not a.archive:
    p.error("--archive is required unless --io-only")
if a.io_mib <= 0 or a.io_iterations <= 0:
    p.error("--io-mib and --io-iterations must be positive")
if a.type == "zfs" and not a.zpool_device:
    p.error("--zpool-device is required for ZFS")
out = Path(a.out)
out.mkdir(exist_ok=True)
mp = Path(a.mountpoint)
mp.mkdir(exist_ok=True)
log = open(out / (a.name + ".jsonl"), "a", buffering=1)


def emit(obj):
    obj["configuration"] = a.name
    print(json.dumps(obj), flush=True)
    log.write(json.dumps(obj) + "\n")


def run(argv, **kw):
    subprocess.run(argv, check=True, **kw)


def mount():
    if a.type == "zfs":
        run(["zpool", "import", "-N", "-d", a.zpool_device,
             a.device.split("/")[0]])
        run(["zfs", "mount", a.device])
    else:
        run(["mount", "-t", a.type, "-o", a.options, a.device, str(mp)])


def unmount():
    if a.type == "zfs":
        # Export drains the pool; the next import has a new ARC identity.
        # A dataset unmount alone would retain cached file data.
        pool = a.device.split("/")[0]
        run(["zpool", "sync", pool])
        run(["zpool", "wait", "-t", "free", pool])
        # The OpenBSD port can transiently retain pool references after the
        # dataset has unmounted. Keep retries visible and inside drain timing.
        deadline = time.monotonic() + 30
        while True:
            argv = ["zpool", "export", pool]
            result = subprocess.run(argv, capture_output=True, text=True,
                                    env={**os.environ, "LC_ALL": "C"})
            if result.returncode == 0:
                break
            if "pool is busy" not in result.stderr or time.monotonic() >= deadline:
                raise subprocess.CalledProcessError(
                    result.returncode, argv, result.stdout, result.stderr)
            emit({"event": "export-retry", "stderr": result.stderr,
                  "sleep_seconds": 0.05})
            time.sleep(0.05)
    else:
        run(["umount", str(mp)])


def checkpoint(label):
    if a.checkpoints:
        emit({"event": "checkpoint", "workload": label})
        if sys.stdin.readline().strip() != "continue":
            raise RuntimeError("checkpoint was not acknowledged")


def observe(label, phase):
    if a.observe:
        emit({"event": "observation", "workload": label, "phase": phase})
        if sys.stdin.readline().strip() != "continue":
            raise RuntimeError("observation was not acknowledged")


def measure(label, argv, durable=False, output=None):
    emit({"event": "start", "workload": label, "argv": argv})
    observe(label, "before")
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    with open(out / (a.name + "-" + label + ".stderr"), "wb") as err:
        with open(output or os.devnull, "wb") as stdout:
            run(argv, stdout=stdout, stderr=err, env={**os.environ, "LC_ALL": "C"})
    elapsed = time.monotonic() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    drain = 0
    if durable:
        # Unmount drains this filesystem and waits for its writes. A global
        # sync would also time unrelated root/NFS activity.
        start = time.monotonic()
        unmount()
        drain = time.monotonic() - start
    observe(label, "after")
    result = {
        "event": "result", "workload": label, "command_seconds": elapsed,
        "unmount_seconds": drain, "total_seconds": elapsed + drain,
        "user_seconds": after.ru_utime - before.ru_utime,
        "system_seconds": after.ru_stime - before.ru_stime,
        "inblock": after.ru_inblock - before.ru_inblock,
        "oublock": after.ru_oublock - before.ru_oublock,
        "drain_method": "sync/wait-free/export" if a.type == "zfs" else "unmount",
    }
    if output:
        result["output_sha256"] = hashlib.sha256(Path(output).read_bytes()).hexdigest()
        result["output_bytes"] = Path(output).stat().st_size
    emit(result)


def delete_tree():
    mount()
    measure("delete", ["rm", "-rf", str(mp / "tree")], durable=True)
    mount()
    assert not (mp / "tree").exists()
    unmount()
    checkpoint("deleted")
    emit({"event": "complete"})


def sequential_io():
    size = "2g" if a.io_mib == 2048 else str(a.io_mib) + "m"
    for iteration in range(1, a.io_iterations + 1):
        mount()
        if a.io_pattern == "zero":
            writer = ["dd", "if=/dev/zero", "of=" + str(mp / "large"),
                      "bs=1048576", "count=" + str(a.io_mib)]
        else:
            writer = [sys.executable, "-c",
                      "import hashlib,sys\n"
                      "data=hashlib.shake_256(b'filesystem-benchmark-v1').digest(1048576)\n"
                      "with open(sys.argv[1], 'wb', buffering=0) as f:\n"
                      " for _ in range(int(sys.argv[2])):\n"
                      "  assert f.write(data) == len(data)\n",
                      str(mp / "large"), str(a.io_mib)]
        measure("write-" + size + "-" + str(iteration), writer, durable=True)
        checkpoint("write-" + str(iteration))
        mount()
        measure("read-" + size + "-" + str(iteration),
                ["dd", "if=" + str(mp / "large"), "of=/dev/null", "bs=1048576"])
        if a.read_repeat:
            measure("read-repeat-" + size + "-" + str(iteration),
                    ["dd", "if=" + str(mp / "large"), "of=/dev/null", "bs=1048576"])
        unmount()
        if a.verify_data:
            mount()
            total = 0
            expected = (bytes(1024 * 1024) if a.io_pattern == "zero" else
                        hashlib.shake_256(b"filesystem-benchmark-v1").digest(1048576))
            with (mp / "large").open("rb") as source:
                while data := source.read(len(expected)):
                    assert data == expected[:len(data)], total
                    total += len(data)
            assert total == a.io_mib * 1024 * 1024, total
            emit({"event": "io-verification", "iteration": iteration,
                  "bytes": total})
            unmount()
        mount()
        measure("delete-large-" + size + "-" + str(iteration),
                ["rm", str(mp / "large")], durable=True)


emit({"event": "setup", "args": vars(a)})
mount()
emit({"event": "mounts", "output": subprocess.check_output(["mount"], text=True)})
assert not (mp / "tree").exists() and not (mp / "large").exists()
if a.io_only:
    unmount()
    sequential_io()
    emit({"event": "complete"})
    raise SystemExit(0)
run(["mkdir", str(mp / "tree")])
measure("extract", ["tar", "-xpf", a.archive, "-C", str(mp / "tree")], durable=True)
checkpoint("extracted")
if a.delete_only:
    delete_tree()
    raise SystemExit(0)
mount()
for label in ["grep-remount", "grep-repeat"]:
    measure(label, ["grep", "-r", "-a", "-F", "-c", "Copyright", str(mp / "tree")],
            output=str(out / (a.name + "-" + label + ".stdout")))
unmount()
mount()
measure("chown", ["chown", "-R", "12345:12345", str(mp / "tree")], durable=True)
mount()
# Change permissions before restoring the public modes. Source archives often
# already use 0644/0755, which would make the restoring chmod a no-op.
measure("chmod-private", ["chmod", "-R", "u=rwX,go=", str(mp / "tree")], durable=True)
mount()
measure("chmod", ["chmod", "-R", "u=rwX,go=rX", str(mp / "tree")], durable=True)
mount()
counts = {"files": 0, "directories": 0, "symlinks": 0, "bytes": 0}
for parent, dirs, files in os.walk(mp / "tree", followlinks=False):
    for name in dirs + files:
        path = Path(parent) / name
        st = path.lstat()
        assert (st.st_uid, st.st_gid) == (12345, 12345), path
        if path.is_symlink():
            counts["symlinks"] += 1
        elif path.is_dir():
            counts["directories"] += 1
            assert st.st_mode & 0o777 == 0o755, path
        else:
            counts["files"] += 1
            counts["bytes"] += st.st_size
            assert st.st_mode & 0o777 in (0o644, 0o755), path
emit({"event": "tree-verification", **counts})
if a.verify_data:
    verified = 0
    with tarfile.open(a.archive, "r:") as archive:
        for member in archive:
            if not member.isfile():
                continue
            path = mp / "tree" / member.name
            assert path.stat().st_size == member.size, path
            with archive.extractfile(member) as expected, path.open("rb") as actual:
                while data := expected.read(1024 * 1024):
                    assert actual.read(len(data)) == data, path
                assert actual.read(1) == b"", path
            verified += 1
    emit({"event": "archive-verification", "files": verified})
unmount()
sequential_io()
delete_tree()
