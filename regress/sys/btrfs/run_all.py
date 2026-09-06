#!/usr/bin/env python3
"""Host orchestration for the disposable OpenBSD btrfs test VM."""
import argparse
from contextlib import contextmanager
from dataclasses import asdict, dataclass
import datetime
import fcntl
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import select
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent


@dataclass(frozen=True)
class Layout:
    nodesize: int
    data: str
    free_space: str
    block_groups: bool
    holes: bool


LAYOUTS = {
    "4k": Layout(4096, "single", "extent", False, False),
    "16k": Layout(16384, "single", "extent", True, True),
    "4k-dup": Layout(4096, "dup", "none", False, False),
    "16k-bitmap": Layout(16384, "dup", "bitmap", False, True),
}


class Failure(Exception):
    pass


def save_json(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    os.replace(temporary, path)


class Console:
    def __init__(self, path, log):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(10)
        self.sock.connect(path)
        self.log = log

    def send(self, command):
        self.sock.sendall(command.encode() + b"\n")

    def until(self, marker, seconds):
        output = b""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if select.select([self.sock], [], [], 0.2)[0]:
                data = self.sock.recv(65536)
                if not data:
                    raise Failure("console disconnected")
                self.log.write(data.decode(errors="replace"))
                self.log.flush()
                output += data
                if marker(output):
                    return output
        raise Failure(f"console timeout; last output: {output[-2000:]!r}")

    def close(self):
        self.sock.close()


class Serial(Console):
    """Continuously save UART output, including panics during a blocked SSH."""
    def __init__(self, path, log):
        super().__init__(path, log)
        self.pending = queue.Queue(maxsize=256)
        self.stopping = threading.Event()
        self.reader = threading.Thread(target=self.receive, daemon=True)
        self.reader.start()

    def receive(self):
        try:
            while not self.stopping.is_set():
                if not select.select([self.sock], [], [], 0.2)[0]:
                    continue
                data = self.sock.recv(65536)
                if not data:
                    return
                self.log.write(data.decode(errors="replace"))
                self.log.flush()
                try:
                    self.pending.put_nowait(data)
                except queue.Full:
                    try:
                        self.pending.get_nowait()
                    except queue.Empty:
                        pass
                    self.pending.put_nowait(data)
        except OSError:
            if not self.stopping.is_set():
                self.log.write("serial connection lost\n")

    def until(self, marker, seconds):
        output = b""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                data = self.pending.get(timeout=0.2)
            except queue.Empty:
                if not self.reader.is_alive():
                    raise Failure("serial connection lost")
                continue
            output = (output + data)[-1024 * 1024:]
            if marker(output):
                return output
        raise Failure(f"serial timeout; last output: {output[-2000:]!r}")

    def close(self):
        self.stopping.set()
        self.reader.join(timeout=2)
        super().close()


class Runner:
    def __init__(self, args, results):
        self.args = args
        self.results = results
        self.image = str(Path(args.image).resolve())
        self.mountpoint = args.mountpoint
        self.vm_tests = args.vm_source.rstrip("/") + "/regress/sys/btrfs"
        self.paused = False
        self.log = None
        self.case_dir = results
        self.layout = None
        self.monitor = None
        self.serial = None
        self.sequence = 0

    def note(self, text):
        line = f"{datetime.datetime.now().isoformat(timespec='seconds')} {text}"
        print(line, flush=True)
        if self.log is not None:
            self.log.write(line + "\n")
            self.log.flush()

    def command(self, argv, *, capture=False, input=None, seconds=None,
                check=True):
        argv = [str(arg) for arg in argv]
        self.log.write("$ " + shlex.join(argv) + "\n")
        self.log.flush()
        try:
            result = subprocess.run(
                argv, input=input, text=True,
                stdout=subprocess.PIPE if capture else self.log,
                stderr=self.log, timeout=seconds or self.args.timeout)
        except subprocess.TimeoutExpired as error:
            raise Failure(f"host command timed out: {shlex.join(argv)}") from error
        if capture:
            self.log.write(result.stdout)
            self.log.flush()
        if check and result.returncode:
            raise Failure(f"exit {result.returncode}: {shlex.join(argv)}")
        return result.stdout if capture else result.returncode

    def ssh_argv(self, argv, seconds=None):
        return ["timeout", "-k", "5", str(seconds or self.args.timeout),
                "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2",
                self.args.vm, shlex.join([str(arg) for arg in argv])]

    def vm(self, *argv, capture=False, input=None, seconds=None, check=True):
        if self.paused:
            raise Failure("attempted SSH while the VM is paused")
        return self.command(self.ssh_argv(argv, seconds), capture=capture,
                            input=input, seconds=(seconds or self.args.timeout) + 10,
                            check=check)

    def vm_python(self, code, *args, **kwargs):
        return self.vm("python3", "-c", code, *args, **kwargs)

    def test(self, script, phase=None, directory=None, *extra):
        argv = ["python3", f"{self.vm_tests}/{script}.py"]
        if phase is not None:
            argv.append(phase)
        argv.append(directory or self.mountpoint)
        return self.vm(*argv, *extra)

    def host_test(self, script, *args, capture=False):
        return self.command(["python3", HERE / f"{script}.py", *args],
                            capture=capture)

    def path(self, name):
        return self.mountpoint.rstrip("/") + "/" + name

    def unmounted(self):
        if self.paused:
            return
        mounts = self.vm("mount", capture=True, seconds=20)
        # Check every partition and view of the configured disk, including
        # mounts made by a regression rather than by this runner.
        disk = re.sub(r"[a-p]$", "", self.args.device)
        for line in mounts.splitlines():
            source = line.split(" on ", 1)[0]
            if source == self.args.device or re.fullmatch(
                    re.escape(disk) + r"[a-p]", source):
                raise Failure(f"test disk is still mounted: {line}")

    def resume_unmount(self):
        mounts = self.vm("mount", capture=True, seconds=20)
        disk = re.sub(r"[a-p]$", "", self.args.device)
        points = []
        for line in mounts.splitlines():
            match = re.match(r"(\S+) on (.+) type \S+ ", line)
            if match and re.fullmatch(re.escape(disk) + r"[a-p]", match[1]):
                point = match[2]
                if not (point == self.mountpoint or
                        point in (self.mountpoint + "-left", self.mountpoint + "-right") or
                        point.startswith(self.mountpoint + "-views/")):
                    raise Failure(f"resume found an unfamiliar mount: {point}")
                points.append(point)
        for point in sorted(points, key=len, reverse=True):
            self.unmount(point)
        self.unmounted()

    def mount(self, mode="rw", point=None, tree=None):
        argv = ["mount_btrfs", "-o", mode]
        if tree is not None:
            argv += ["-s", str(tree)]
        self.vm(*argv, self.args.device, point or self.mountpoint)

    def unmount(self, point=None):
        self.vm("umount", point or self.mountpoint)

    def checks(self):
        self.unmounted()
        self.command(["btrfs", "check", "--readonly", "--check-data-csum",
                      self.image])
        self.host_test("mirrors", self.image)

    def seed(self, script, phase="seed"):
        self.sequence += 1
        directory = self.case_dir / f"seed-{self.sequence}"
        self.host_test(script, phase, directory)
        return directory

    def format(self, seed=None, *, size=None, data=None, free_space=None,
               block_groups=None, holes=None, extref=True, compress=None,
               subvols=(), flags=(), system=False, nodesize=None):
        self.unmounted()
        layout = self.layout
        data = data or layout.data
        free_space = free_space or layout.free_space
        if block_groups is None:
            block_groups = layout.block_groups
        if holes is None:
            holes = layout.holes
        features = [
            "free-space-tree" if free_space != "none" else "^free-space-tree",
            "block-group-tree" if block_groups else "^block-group-tree",
            "^no-holes" if holes else "no-holes",
            "extref" if extref else "^extref",
        ]
        if block_groups and free_space == "none":
            raise Failure("block-group tree requires free-space tree")
        argv = ["mkfs.btrfs", "-f", "-b", size or "256M", "-s", "4096",
                "-n", str(nodesize or layout.nodesize), "-d", data, "-m", "dup",
                "-O", ",".join(features)]
        if seed is not None:
            argv += ["--rootdir", str(seed)]
        if compress:
            argv += ["--compress", compress]
        for subvol in subvols:
            argv += ["--subvol", subvol]
        for flag in flags:
            argv += ["--inode-flags", flag]
        self.command([*argv, self.image])
        if system:
            self.host_test("chunks_fixture", self.image)
        if free_space == "bitmap":
            self.host_test("free_space", "bitmap-groups", self.image)
        self.checks()

    def restore(self, script, phase):
        self.unmounted()
        self.sequence += 1
        directory = self.case_dir / f"restore-{self.sequence}"
        directory.mkdir()
        self.command(["btrfs", "restore", self.image, directory])
        self.host_test(script, phase, directory)

    def inspect(self, *args):
        self.unmounted()
        return self.command(["btrfs", "inspect-internal", *args, self.image],
                            capture=True)

    def monitor_command(self, command):
        if self.monitor is None:
            self.monitor = Console(self.args.monitor, self.log)
            self.monitor.until(lambda b: b.rstrip().endswith(b"(qemu)"), 10)
        self.monitor.send(command)
        return self.monitor.until(lambda b: b.rstrip().endswith(b"(qemu)"), 10)

    def reset_paused(self):
        self.monitor_command("stop")
        self.monitor_command("system_reset")
        status = self.monitor_command("info status")
        if b"paused" not in status:
            raise Failure("QEMU did not remain paused after reset")
        self.paused = True

    def boot(self):
        self.monitor_command("cont")
        self.paused = False
        deadline = time.monotonic() + self.args.boot_timeout
        while time.monotonic() < deadline:
            if self.vm("true", seconds=10, check=False) == 0:
                self.unmounted()
                return
            time.sleep(2)
        raise Failure("VM did not return after reset")

    @contextmanager
    def background(self, argv):
        argv = self.ssh_argv(argv)
        self.log.write("$ " + shlex.join(argv) + " &\n")
        self.log.flush()
        process = subprocess.Popen(argv, stdout=self.log, stderr=self.log,
                                   start_new_session=True)
        try:
            yield process
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()

    def test_argv(self, script, phase, directory):
        return ["python3", f"{self.vm_tests}/{script}.py", phase, directory]

    def crash_ready(self, script, directory, marker):
        self.vm("rm", "-f", marker)
        with self.background(self.test_argv(script, "hold", directory)) as proc:
            deadline = time.monotonic() + self.args.timeout
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise Failure("hold process exited before its ready marker")
                if self.vm("test", "-f", marker, check=False, seconds=10) == 0:
                    self.reset_paused()
                    return
                time.sleep(1)
            raise Failure(f"no ready marker: {marker}")

    def crash_break(self, argv, targets, hits=1):
        serial = self.serial
        prompt = lambda b: re.search(rb"ddb\{\d+\}>\s*$", b) is not None

        def proceed():
            # amd64 DDB presents its prompt before secondary CPUs necessarily
            # receive their stop IPIs. Resuming too soon can let a delayed IPI
            # enter DDB again instead of stopping at the requested breakpoint.
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                serial.send("machine cpuinfo")
                answer = serial.until(prompt, 20)
                states = re.findall(
                    rb"(?:^|[\r\n])[* ]+\d+: ([^\r\n]+)", answer)
                if states and all(s in (b"stopped", b"ddb") for s in states):
                    serial.send("continue")
                    return
                time.sleep(0.1)
            raise Failure("DDB secondary CPUs did not stop")

        def breakpoint(target):
            deadline = time.monotonic() + self.args.timeout
            while time.monotonic() < deadline:
                answer = serial.until(prompt, deadline - time.monotonic())
                if re.search(rb"Breakpoint at\s+" + target.encode() + rb"[:+]",
                             answer):
                    return
                # A stop IPI can also arrive during DDB's invisible step over
                # the previous breakpoint. It is not another filesystem hit.
                if re.search(rb"Stopped at\s+x86_ipi_db\+0x[0-9a-f]+:", answer):
                    self.note("DDB stop IPI; continuing without counting a hit")
                    proceed()
                    continue
                raise Failure(f"unexpected debugger stop: {target}")
            raise Failure(f"breakpoint timeout: {target}")

        self.monitor_command("nmi")
        serial.until(prompt, 20)
        serial.send("break " + targets[0])
        answer = serial.until(prompt, 20)
        if b"Symbol not found" in answer:
            raise Failure("kernel lacks requested DDB symbol")
        proceed()
        # Launch immediately while owning the console. Separate manual
        # launches can leave the debugger waiting past the harness timeout.
        with self.background(argv):
            for number, target in enumerate(targets):
                stop_count = hits if number == len(targets) - 1 else 1
                for hit in range(stop_count):
                    breakpoint(target)
                    if hit + 1 < stop_count:
                        proceed()
                if number + 1 < len(targets):
                    serial.send("delete " + target)
                    serial.until(prompt, 20)
                    serial.send("break " + targets[number + 1])
                    answer = serial.until(prompt, 20)
                    if b"Symbol not found" in answer:
                        raise Failure("kernel lacks requested DDB symbol")
                    proceed()
            self.reset_paused()

    def diagnostics(self):
        try:
            self.monitor_command("info status")
            time.sleep(1)  # Allow the UART reader to drain pending output.
        except (OSError, Failure) as error:
            self.log.write(f"diagnostics: {error}\n")


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm", default="root@10.77.0.2")
    parser.add_argument("--image", default="/home/mike/obj/btrfs.img",
                        help="existing disposable backing file; reformatted per case")
    parser.add_argument("--device", default="/dev/sd1c")
    parser.add_argument("--mountpoint", default="/mnt/btrfs")
    parser.add_argument("--vm-source", default="/mnt/src")
    parser.add_argument("--serial", default="/tmp/serial.sock")
    parser.add_argument("--monitor", default="/tmp/monitor.sock")
    parser.add_argument("--layouts", default="all",
                        help="comma-separated layout names; default: all")
    parser.add_argument("--select", action="append", default=[],
                        help="case-name or layout/case glob; repeat to select a union")
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--start-at",
                        help="start at this case or layout/case in the selected plan")
    parser.add_argument("--list", action="store_true", help="list cases without VM access")
    parser.add_argument("--results", type=Path,
                        help="log/fixture directory (default: next to backing image)")
    parser.add_argument("--resume", action="store_true",
                        help="skip passed cases in --results; restart incomplete cases")
    parser.add_argument("--timeout", type=int, default=900,
                        help="seconds per command/breakpoint (default: 900)")
    parser.add_argument("--boot-timeout", type=int, default=180)
    args = parser.parse_args()
    if args.timeout <= 0 or args.boot_timeout <= 0:
        parser.error("timeouts must be positive")
    if args.resume and args.results is None:
        parser.error("--resume requires --results")
    return args


def main():
    from run_cases import cases
    args = arguments()
    layouts = list(LAYOUTS) if args.layouts == "all" else args.layouts.split(",")
    if not set(layouts) <= LAYOUTS.keys():
        raise Failure("unknown layout; choose from " + ", ".join(LAYOUTS))
    catalog = cases()
    plan = []
    for layout in layouts:
        for name, function in catalog.items():
            identity = f"{layout}/{name}"
            matches = lambda patterns: any(
                fnmatch.fnmatchcase(name, p) or fnmatch.fnmatchcase(identity, p)
                for p in patterns)
            if (not args.select or matches(args.select)) and not matches(args.exclude):
                plan.append((identity, layout, function))
    if not plan:
        raise Failure("selection matched no cases")
    if args.start_at:
        for index, (identity, _, _) in enumerate(plan):
            if args.start_at in (identity, identity.split("/", 1)[1]):
                plan = plan[index:]
                break
        else:
            raise Failure("--start-at did not match a selected case")
    if args.list:
        for identity, _, _ in plan:
            print(identity)
        print(f"{len(plan)} cases")
        return 0
    for tool in ("ssh", "timeout", "mkfs.btrfs", "btrfs", "python3"):
        if shutil.which(tool) is None:
            raise Failure(f"missing host tool: {tool}")
    image = Path(args.image).resolve()
    if not image.is_file() or not stat.S_ISREG(image.stat().st_mode):
        raise Failure("image must be an existing regular backing file")
    results = args.results or image.parent / (
        "btrfs-regress-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    results = results.resolve()
    if results.exists() and not args.resume:
        raise Failure("results directory exists; use --resume or a new directory")
    results.mkdir(parents=True, exist_ok=True)
    runner = Runner(args, results)
    # Hold both locks for the complete run, including all subprocesses.
    with image.open("rb") as image_lock, open(args.monitor + ".runner-lock", "w") as vm_lock:
        for lock in (image_lock, vm_lock):
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise Failure("another runner owns this image or VM") from error
        with (results / "preflight.log").open("a", buffering=1) as log:
            runner.log = log
            runner.vm_python(
                "import os,sys; assert os.geteuid()==0; "
                "assert os.path.isfile(sys.argv[1]); "
                "assert os.path.exists(sys.argv[2]); "
                "os.makedirs(sys.argv[3],exist_ok=True)",
                runner.vm_tests + "/namespace.py", args.device, args.mountpoint)
            kernel = runner.vm("uname", "-v", capture=True).strip()
            runner.monitor_command("info status")
            blocks = runner.monitor_command("info block")
            if str(image).encode() not in blocks:
                raise Failure("configured backing image is not attached to this VM")
            runner.monitor.close()
            runner.monitor = None
            digest = hashlib.sha256()
            for path in sorted([*HERE.glob("*.py"), HERE / "run-all.sh"]):
                digest.update(path.name.encode())
                digest.update(path.read_bytes())
            config = {
                "vm": args.vm, "image": str(image), "device": args.device,
                "mountpoint": args.mountpoint, "vm_source": args.vm_source,
                "serial": args.serial, "monitor": args.monitor,
                "kernel": kernel, "suite_sha256": digest.hexdigest(),
                "layouts": {name: asdict(LAYOUTS[name]) for name in layouts},
                "cases": [identity for identity, _, _ in plan],
            }
            config_path = results / "config.json"
            if args.resume:
                if json.loads(config_path.read_text()) != config:
                    raise Failure("resume configuration, kernel or suite changed; "
                                  "use a new results directory and --select")
                runner.resume_unmount()
            else:
                runner.unmounted()
                save_json(config_path, config)
        status_path = results / "status.json"
        statuses = json.loads(status_path.read_text()) if status_path.exists() else {}
        for identity, layout, function in plan:
            if statuses.get(identity, {}).get("status") == "passed":
                print(f"RESUME {identity}: already passed", flush=True)
                continue
            # Keep failed-attempt artifacts when restarting an incomplete case.
            parent = results / identity
            parent.mkdir(parents=True, exist_ok=True)
            attempt = 1
            while (parent / str(attempt)).exists():
                attempt += 1
            runner.case_dir = parent / str(attempt)
            runner.case_dir.mkdir()
            runner.layout = LAYOUTS[layout]
            runner.sequence = 0
            start = time.monotonic()
            with (runner.case_dir / "commands.log").open("w", buffering=1) as log, \
                    (runner.case_dir / "serial.log").open("w", buffering=1) as serial_log:
                runner.log = log
                runner.note(f"START {identity}; log: {log.name}")
                statuses[identity] = {"status": "running", "log": log.name}
                save_json(status_path, statuses)
                try:
                    runner.serial = Serial(args.serial, serial_log)
                    function(runner)
                    runner.unmounted()
                except (Exception, KeyboardInterrupt) as error:
                    runner.note(f"FAIL {identity}: {error}")
                    runner.diagnostics()
                    statuses[identity].update(status="failed", error=str(error),
                                               seconds=time.monotonic() - start)
                    save_json(status_path, statuses)
                    runner.note("Stopped; backing image and VM state preserved.")
                    return 1
                finally:
                    if runner.serial is not None:
                        runner.serial.close()
                        runner.serial = None
                    if runner.monitor is not None:
                        runner.monitor.close()
                        runner.monitor = None
                statuses[identity].update(status="passed",
                                           seconds=time.monotonic() - start)
                save_json(status_path, statuses)
                runner.note(f"PASS {identity} ({time.monotonic() - start:.1f}s)")
        print(f"Passed {len(plan)} cases. Results: {results}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, OSError, ValueError) as error:
        sys.exit(f"run-all: {error}")
