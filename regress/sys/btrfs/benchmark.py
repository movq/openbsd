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
p.add_argument("--archive", required=True)
p.add_argument("--out", required=True)
a = p.parse_args()
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


emit({"event": "setup", "args": vars(a)})
mount()
assert not (mp / "tree").exists() and not (mp / "large").exists()
run(["mkdir", str(mp / "tree")])
measure("extract", ["tar", "-xpf", a.archive, "-C", str(mp / "tree")], durable=True)
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
for iteration in (1, 2):
    mount()
    measure("write-2g-" + str(iteration),
            ["dd", "if=/dev/zero", "of=" + str(mp / "large"), "bs=1m", "count=2048"],
            durable=True)
    mount()
    measure("read-2g-" + str(iteration),
            ["dd", "if=" + str(mp / "large"), "of=/dev/null", "bs=1m"])
    unmount()
    mount()
    run(["rm", str(mp / "large")])
    unmount()
mount()
measure("delete", ["rm", "-rf", str(mp / "tree")], durable=True)
mount()
assert not (mp / "tree").exists()
emit({"event": "complete"})
unmount()
