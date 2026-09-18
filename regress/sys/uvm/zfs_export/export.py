#!/usr/bin/env python3
"""Stress ARC teardown by repeatedly importing, reading and exporting a pool.

Requires an exported disposable pool, a dataset with a configured mountpoint,
and an existing extracted source archive under MOUNTPOINT/tree. No pool or
dataset is created or destroyed. Run under an external timeout; a kernel panic
can block the process. The successful result leaves the pool exported.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("dataset")
p.add_argument("device")
p.add_argument("mountpoint", type=Path)
p.add_argument("archive")
p.add_argument("--iterations", type=int, default=12)
a = p.parse_args()
if a.iterations <= 0:
    p.error("--iterations must be positive")
pool = a.dataset.split("/")[0]


def run(argv):
    subprocess.run(argv, check=True, env={**os.environ, "LC_ALL": "C"})


manifest = {}
with tarfile.open(a.archive, "r:") as archive:
    for member in archive:
        if member.isfile():
            with archive.extractfile(member) as source:
                manifest[member.name] = (member.size,
                                        hashlib.file_digest(source, "sha256").digest())

for iteration in range(a.iterations):
    start = time.monotonic()
    run(["zpool", "import", "-N", "-d", a.device, pool])
    run(["zfs", "mount", a.dataset])
    for name, (size, digest) in manifest.items():
        path = a.mountpoint / "tree" / name
        assert path.stat().st_size == size, path
        with path.open("rb") as source:
            assert hashlib.file_digest(source, "sha256").digest() == digest, path
    run(["zpool", "sync", pool])
    # Keep the initial port's transient EBUSY visible; never force an export.
    retries = 0
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
        retries += 1
        time.sleep(0.05)
    print(json.dumps({"iteration": iteration + 1, "verified_files": len(manifest),
                      "export_retries": retries,
                      "seconds": time.monotonic() - start}), flush=True)

print(json.dumps({"event": "complete", "iterations": a.iterations}), flush=True)
