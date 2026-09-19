#!/usr/bin/env python3
"""Format one disposable VM disk and run benchmark.py with QEMU counters.

The selected device is destroyed. Run under an external timeout. Other VMs
sharing the backing image must leave it unmounted for the entire run.
"""
import argparse
import fcntl
import json
from pathlib import Path
import re
import shlex
import socket
import struct
import subprocess


p = argparse.ArgumentParser(description=__doc__)
p.add_argument("name")
p.add_argument("--vm", required=True)
p.add_argument("--monitor", required=True)
p.add_argument("--image", required=True, type=Path)
p.add_argument("--block", required=True, help="QEMU block backend name")
p.add_argument("--device", required=True, help="disposable OpenBSD whole disk")
p.add_argument("--type", choices=["btrfs", "zfs"], required=True)
p.add_argument("--format-scratch", action="store_true", required=True)
p.add_argument("--mountpoint", default="/mnt/bench")
p.add_argument("--pool", default="fsbench")
p.add_argument("--runner", default="/usr/src/regress/sys/btrfs/benchmark.py")
p.add_argument("--archive", required=True)
p.add_argument("--guest-out", required=True)
p.add_argument("--out", required=True, type=Path)
p.add_argument("--io-mib", type=int, default=16384)
p.add_argument("--io-iterations", type=int, default=1)
p.add_argument("--verify-data", action="store_true")
a = p.parse_args()
if not re.fullmatch(r"/dev/sd[1-9][0-9]*c", a.device):
    p.error("--device must be a non-root whole disk, e.g. /dev/sd1c")
a.out.mkdir(parents=True, exist_ok=False)


def ssh(argv, **kwargs):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", a.vm,
                           shlex.join(argv)], check=True, **kwargs)


def monitor(command):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(15)
        s.connect(a.monitor)

        def receive():
            data = b""
            while not data.endswith(b"(qemu) "):
                chunk = s.recv(65536)
                if not chunk:
                    raise RuntimeError("QEMU monitor closed")
                data += chunk
            return data.decode(errors="replace")

        receive()
        s.sendall((command + "\n").encode())
        return receive()


def counters():
    text = monitor("info blockstats")
    match = re.search(r"(?:^|\n)" + re.escape(a.block) + r": ([^\r\n]+)", text)
    if not match:
        raise RuntimeError("QEMU block backend missing: " + text)
    result = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", match[1])
              if k != "idle_time_ns"}
    if a.type == "btrfs":
        # The raw image is shared with QEMU. At observation boundaries,
        # generation differences count completed full transactions; cache
        # flushes and tree-log publications are not transaction counts.
        with a.image.open("rb") as image:
            image.seek(65536 + 64)
            magic, generation = struct.unpack("<8sQ", image.read(16))
        if magic != b"_BHRfS_M":
            raise RuntimeError("invalid Btrfs superblock")
        result["super_generation"] = generation
    return result


def delta(before, after):
    result = {k: after[k] - v for k, v in before.items()}
    if any(v < 0 for v in result.values()):
        raise RuntimeError("QEMU counters went backwards")
    if "super_generation" in result:
        result["transaction_commits"] = result.pop("super_generation")
    result["wr_sectors_512"] = result["wr_bytes"] // 512
    result["wr_blocks_4096"] = result["wr_bytes"] / 4096
    return result


with (open(a.monitor + ".runner-lock", "a") as vm_lock,
      a.image.open("rb") as image_lock,
      (a.out / "setup.log").open("w", buffering=1) as setup):
    for lock in (vm_lock, image_lock):
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    blocks = monitor("info block")
    setup.write(blocks)
    if not re.search(re.escape(a.block) + r" .*?: " +
                     re.escape(str(a.image.resolve())) + r" \(raw\)", blocks):
        raise RuntimeError("backing image does not match QEMU backend")
    mounts = ssh(["mount"], capture_output=True, text=True).stdout
    setup.write(mounts)
    if a.device[:-1] in mounts or " on " + a.mountpoint + " " in mounts:
        raise RuntimeError("scratch device or mountpoint is in use")
    pools = ssh(["zpool", "status", "-P"], capture_output=True, text=True)
    setup.write(pools.stdout + pools.stderr)
    if a.device[:-1] in pools.stdout:
        raise RuntimeError("scratch device belongs to an imported pool")
    for argv in (["uname", "-a"], ["sha256", "/bsd"],
                 ["sysctl", "hw.physmem", "hw.ncpu", "kern.bufcachepercent"],
                 ["disklabel", a.device], ["zfs", "version"],
                 ["btrfs", "version"]):
        ssh(argv, stdout=setup, stderr=setup)
    ssh(["mkdir", "-p", a.mountpoint, a.guest_out])

    def configure(argv):
        setup.write("$ " + shlex.join(argv) + "\n")
        ssh(argv, stdout=setup, stderr=setup)

    if a.type == "btrfs":
        configure(["mkfs.btrfs", "-f", "-K", "-n", "16384", "-s", "4096",
                   "-d", "single", "-m", "dup", "-O",
                   "free-space-tree,^block-group-tree,no-holes",
                   "--checksum", "crc32c", a.device])
        device = a.device
    else:
        configure(["zpool", "create", "-f", "-o", "ashift=12",
                   "-o", "cachefile=none", "-o", "autotrim=off",
                   "-O", "mountpoint=none", "-O", "compression=off",
                   "-O", "atime=off", "-O", "dedup=off", "-O", "copies=1",
                   "-O", "recordsize=128K", "-O", "checksum=fletcher4",
                   "-O", "redundant_metadata=all", "-O", "sync=standard",
                   a.pool, a.device])
        device = a.pool + "/bench"
        configure(["zfs", "create", "-o", "mountpoint=" + a.mountpoint, device])
        configure(["zpool", "get", "all", a.pool])
        configure(["zfs", "get", "all", device])
        configure(["zpool", "status", "-P", a.pool])
        configure(["zpool", "export", a.pool])

    argv = ["python3", a.runner, a.name, device, a.mountpoint,
            "--type", a.type, "--options", "rw,noatime",
            "--archive", a.archive, "--out", a.guest_out,
            "--io-mib", str(a.io_mib), "--io-iterations", str(a.io_iterations),
            "--io-pattern", "pattern", "--observe"]
    if a.type == "zfs":
        argv.extend(["--zpool-device", a.device])
    if a.verify_data:
        argv.append("--verify-data")
    configure(["dd", "if=" + a.archive, "of=/dev/null", "bs=1048576"])
    setup.write("$ " + shlex.join(argv) + "\n")
    initial = counters()
    (a.out / "config.json").write_text(json.dumps(
        {**{k: str(v) if isinstance(v, Path) else v for k, v in vars(a).items()},
         "command": argv, "initial_counters": initial}, indent=2) + "\n")
    before = {}
    measured = {}
    complete = False
    with ((a.out / "results.jsonl").open("w", buffering=1) as log,
          (a.out / "guest.stderr").open("w") as err):
        proc = subprocess.Popen(["ssh", "-o", "BatchMode=yes", a.vm,
                                 shlex.join(argv)], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=err,
                                text=True, bufsize=1)
        try:
            for line in proc.stdout:
                event = json.loads(line)
                if event["event"] == "observation":
                    now = counters()
                    event["device_counters"] = now
                    label = event["workload"]
                    if event["phase"] == "before":
                        before[label] = now
                    else:
                        measured[label] = delta(before[label], now)
                    proc.stdin.write("continue\n")
                    proc.stdin.flush()
                elif event["event"] == "result":
                    event["device"] = measured[event["workload"]]
                    print(json.dumps(event), flush=True)
                elif event["event"] == "complete":
                    complete = True
                log.write(json.dumps(event) + "\n")
            if proc.wait() != 0 or not complete:
                raise RuntimeError("guest benchmark failed; see guest.stderr")
        finally:
            proc.stdin.close()
            if proc.poll() is None:
                proc.terminate()
                proc.wait()
        final = counters()
        total = {"event": "run-device-total", "device": delta(initial, final),
                 "final_counters": final}
        log.write(json.dumps(total) + "\n")
        print(json.dumps(total), flush=True)
