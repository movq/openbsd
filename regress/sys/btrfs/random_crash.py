#!/usr/bin/env python3
"""Destructive random-reset durability test on a disposable OpenBSD VM.

Reformats the selected scratch image. The image must be attached to this VM
only, as the selected device. Requires host btrfs-progs and guest Python 3.
"""
import argparse
import datetime
import os
from pathlib import Path
import random
import re
import shlex
import signal
import subprocess
import time

from random_crash_workload import acknowledgements
from run_all import Failure, LAYOUTS, Runner, Serial, save_json


def terminate(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def trial(r, index, rng):
    args = r.args
    directory = r.results / f"{index:04d}"
    directory.mkdir()
    seed = rng.getrandbits(64)
    delay = rng.uniform(args.min_delay, args.max_delay)
    mode = ("append", "namespace", "mixed")[index % 3]
    details = dict(seed=seed, delay=delay, mode=mode, workers=args.workers)
    save_json(directory / "trial.json", details)
    r.note(f"trial {index}: {mode}, seed {seed}, reset after {delay:.3f}s")
    base = r.path("random-crash")

    def workload(phase):
        return [*r.test_argv("random_crash_workload", phase, base),
                "--seed", str(seed), "--workers", str(args.workers),
                "--mode", mode]

    r.mount()
    r.vm("rm", "-rf", base)
    r.vm(*workload("initialize"))
    ledger = directory / "acknowledgements.txt"
    serial_start = (r.results / "serial.log").stat().st_size
    with ledger.open("w") as output, (directory / "writer.stderr").open("w") as err:
        argv = r.ssh_argv(workload("write"))
        r.log.write("$ " + shlex.join(argv) + "\n")
        r.log.flush()
        process = subprocess.Popen(argv, stdout=output, stderr=err,
                                   start_new_session=True)
        try:
            deadline = time.monotonic() + args.timeout
            while "READY\n" not in ledger.read_text():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise Failure("writer failed to become ready")
                time.sleep(0.01)
            started = time.monotonic()
            time.sleep(delay)
            if process.poll() is not None:
                raise Failure("writer exited before reset")
            # No guest sync/unmount or DDB breakpoint. Reset stops guest RAM
            # activity; QEMU and the host's completed disk writes survive.
            r.reset_paused()
            details["elapsed"] = time.monotonic() - started
            r.command(["cp", "--reflink=auto", "--sparse=always", r.image,
                       directory / "crash.img"])
            # Give SSH time to deliver output already received before reset.
            time.sleep(0.2)
        finally:
            terminate(process)
    text = ledger.read_text()
    # An interrupted SSH packet can leave an incomplete final line. Only
    # complete host-observed acknowledgements count as durability promises.
    text = text[:text.rfind("\n") + 1]
    (directory / "verified-acknowledgements.txt").write_text(text)
    details["acknowledged_epochs"] = [
        epoch + 1 for epoch in acknowledgements(text, args.workers)]
    save_json(directory / "trial.json", details)
    with (r.results / "serial.log").open() as serial:
        serial.seek(serial_start)
        output = serial.read()
    if re.search(r"panic:|Stopped at|ddb\{", output):
        raise Failure("guest entered DDB before the planned reset")
    if "Traceback" in (directory / "writer.stderr").read_text():
        raise Failure("guest workload failed before the planned reset")
    # Preserve the raw pre-replay superblocks and independently check their
    # committed trees. This does not validate replayed data until after mount.
    image = directory / "crash.img"
    supers = r.command(["btrfs", "inspect-internal", "dump-super", "-a", image],
                       capture=True)
    (directory / "superblocks.txt").write_text(supers)
    r.command(["btrfs", "check", "--readonly", "--check-data-csum", image])
    r.boot()
    r.mount()
    r.vm(*workload("verify"), input=text)
    r.unmount()
    r.checks()
    r.mount("ro")
    r.vm(*workload("verify"), input=text)
    r.unmount()
    details["passed"] = True
    save_json(directory / "trial.json", details)
    r.note(f"trial {index} passed: {sum(details['acknowledged_epochs'])} epochs")


def run(args):
    args.results.mkdir(parents=True, exist_ok=False)
    save_json(args.results / "arguments.json",
              {key: str(value) if isinstance(value, Path) else value
               for key, value in vars(args).items()})
    r = Runner(args, args.results)
    r.layout = LAYOUTS[args.layout]
    with (args.results / "commands.log").open("w", buffering=1) as log, (
            args.results / "serial.log").open("w", buffering=1) as serial_log:
        r.log = log
        r.serial = Serial(args.serial, serial_log)
        try:
            r.unmounted()
            if "type btrfs" in r.vm("mount", capture=True):
                raise Failure("all btrfs filesystems must be unmounted at entry")
            # Confirm that the backing file is attached, and record its cache
            # configuration. The device-to-image mapping remains a caller
            # precondition, just as in run-all.sh.
            blocks = r.monitor_command("info block").decode(errors="replace")
            if r.image not in blocks:
                raise Failure("selected image is not attached to this QEMU")
            r.vm("python3", "--version")
            r.vm("uname", "-a")
            r.vm("mkdir", "-p", args.mountpoint)
            r.format(size="1G")
            rng = random.Random(args.seed)
            for index in range(args.rounds):
                trial(r, index, rng)
            r.note(f"all {args.rounds} random resets passed; results: {args.results}")
        except BaseException:
            r.note("FAILED: preserving image, mount/pause state, and logs")
            raise
        finally:
            r.serial.close()
            if r.monitor is not None:
                r.monitor.close()


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm", default="root@10.77.0.4")
    parser.add_argument("--image", default="/home/mike/obj/vm/scratch1.img")
    parser.add_argument("--device", default="/dev/sd1c")
    parser.add_argument("--mountpoint", default="/mnt/btrfs-random")
    parser.add_argument("--vm-source", default="/usr/src")
    parser.add_argument("--monitor",
                        default="/home/mike/obj/vm/openbsd-monitor.sock")
    parser.add_argument("--serial",
                        default="/home/mike/obj/vm/openbsd-serial.sock")
    parser.add_argument("--layout", choices=LAYOUTS, default="4k")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--rounds", type=int, default=12)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--min-delay", type=float, default=0.05)
    parser.add_argument("--max-delay", type=float, default=3.0)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--boot-timeout", type=int, default=180)
    parser.add_argument("--results", type=Path, default=Path(
        "/tmp/btrfs-random-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    args = parser.parse_args()
    if args.rounds < 1 or args.workers < 1:
        parser.error("--rounds and --workers must be positive")
    if not 0 <= args.min_delay <= args.max_delay < args.timeout - 30:
        parser.error("require 0 <= min-delay <= max-delay < timeout - 30")
    return args


if __name__ == "__main__":
    run(arguments())
