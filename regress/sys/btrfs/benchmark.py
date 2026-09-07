#!/usr/bin/env python3
"""OpenBSD workload runner for a freshly formatted, unmounted scratch FS.

Creates and removes "tree" and "large" beneath the mountpoint. Formatting is
deliberately external; see BENCHMARKS.md for the measured setup. Run under an
external timeout, since a kernel hang can also block the Python process.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import time


p = argparse.ArgumentParser()
p.add_argument("name")
p.add_argument("device")
p.add_argument("mountpoint")
p.add_argument("--type", choices=["ffs", "btrfs"], required=True)
p.add_argument("--options", default="noatime")
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
a = p.parse_args()
if not a.io_only and not a.archive:
    p.error("--archive is required unless --io-only")
if a.io_mib <= 0 or a.io_iterations <= 0:
    p.error("--io-mib and --io-iterations must be positive")
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
    run(["mount", "-t", a.type, "-o", a.options, a.device, str(mp)])


def unmount():
    run(["umount", str(mp)])


def measure(label, argv, durable=False, output=None):
    emit({"event": "start", "workload": label, "argv": argv})
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
    result = {
        "event": "result", "workload": label, "command_seconds": elapsed,
        "unmount_seconds": drain, "total_seconds": elapsed + drain,
        "user_seconds": after.ru_utime - before.ru_utime,
        "system_seconds": after.ru_stime - before.ru_stime,
        "inblock": after.ru_inblock - before.ru_inblock,
        "oublock": after.ru_oublock - before.ru_oublock,
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
    emit({"event": "complete"})
    unmount()


def sequential_io():
    size = "2g" if a.io_mib == 2048 else str(a.io_mib) + "m"
    for iteration in range(1, a.io_iterations + 1):
        mount()
        measure("write-" + size + "-" + str(iteration),
                ["dd", "if=/dev/zero", "of=" + str(mp / "large"), "bs=1m",
                 "count=" + str(a.io_mib)], durable=True)
        mount()
        measure("read-" + size + "-" + str(iteration),
                ["dd", "if=" + str(mp / "large"), "of=/dev/null", "bs=1m"])
        unmount()
        mount()
        run(["rm", str(mp / "large")])
        unmount()


emit({"event": "setup", "args": vars(a)})
mount()
assert not (mp / "tree").exists() and not (mp / "large").exists()
if a.io_only:
    unmount()
    sequential_io()
    emit({"event": "complete"})
    raise SystemExit(0)
run(["mkdir", str(mp / "tree")])
measure("extract", ["tar", "-xpf", a.archive, "-C", str(mp / "tree")], durable=True)
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
unmount()
sequential_io()
delete_tree()
