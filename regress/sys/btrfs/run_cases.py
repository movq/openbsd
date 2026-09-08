"""Explicit fixture/phase recipes for run_all.py. Each case owns a fresh image."""
from functools import partial
import re


SUBVOLS = ("rw:left", "rw:left/nested", "ro:left/frozen", "rw:right")


def small_size(r):
    return "128M" if r.layout.data == "single" else "256M"


def finish(r, script=None, directory=None, verify="verify", readonly=None):
    r.unmount()
    r.checks()
    for mode in ("ro", "rw"):
        r.mount(mode)
        phase = readonly if mode == "ro" and readonly else verify
        if script and phase:
            r.test(script, phase, directory)
        r.unmount()
    r.checks()


def basic(r, script, create="create", verify="verify", readonly=None,
          **format_args):
    r.format(**format_args)
    r.mount()
    directory = r.path("test")
    r.test(script, create, directory)
    finish(r, script, directory, verify, readonly)


def write_batch_capacity(r):
    basic(r, "write_batch", create="capacity", size=small_size(r))


def cluster(r):
    phase = "create" if (r.layout.nodesize == 4096 and
                         r.layout.data == "single" and
                         r.layout.free_space != "bitmap") else "measure"
    basic(r, "cluster", phase)


def coalesce(r):
    r.format()
    directory = r.path("test")
    r.mount()
    r.test("coalesce", "create", directory)
    r.unmount()
    r.checks()
    r.host_test("coalesce", "disk", r.image)
    r.mount()
    r.test("coalesce", "verify_original", directory)
    r.test("coalesce", "mutate", directory)
    finish(r, "coalesce", directory)


def reflink(r):
    r.format()
    directory = r.path("test")
    r.mount()
    r.test("reflink", "create", directory)
    r.unmount()
    r.checks()
    r.host_test("reflink", "disk", r.image)
    r.mount()
    r.test("reflink", "verify", directory)
    r.test("reflink", "mutate", directory)
    r.test("reflink", "race", r.path("race"))
    finish(r, "reflink", directory, "verify_mutated")


def read_cluster(r):
    r.format()
    directory = r.path("test")
    r.mount()
    r.test("read_cluster", "create", directory)
    r.unmount()
    r.checks()
    r.mount()
    r.test("read_cluster", "measure", directory)
    r.test("read_cluster", "small", directory)
    r.unmount()
    r.checks()
    r.mount()
    r.test("read_cluster", "verify_small", directory)
    r.test("read_cluster", "again", directory)
    finish(r, "read_cluster", directory, "verify_again")


def read_import(r, compressed=False, checksum="crc32c"):
    seed = r.seed("read_cluster", "seed_compressed" if compressed else "seed")
    r.format(seed, compress="zstd" if compressed else None, checksum=checksum)
    if compressed:
        r.host_test("read_cluster", "disk_compressed", r.image)
    r.mount()
    r.test("read_cluster", "compressed" if compressed else "measure")
    if not compressed:
        r.test("read_cluster", "split")
    finish(r, "read_cluster", verify="compressed" if compressed else "verify_split")


def read_faults(r, checksum="crc32c"):
    # Bitmap DUP imports can exceed one 4 KiB free-space leaf.
    r.format(data="dup", free_space="extent", checksum=checksum)
    directory = r.path("test")
    r.mount()
    r.test("read_cluster", "create", directory)
    r.unmount()
    r.checks()
    journal = r.case_dir / "damage.json"
    r.host_test("read_cluster", "damage", r.image, journal)
    r.mount("ro")
    r.test("read_cluster", "faults", directory)
    r.unmount()
    # A failed test leaves the damaged image and journal intact for diagnosis.
    r.host_test("read_cluster", "repair", r.image, journal)
    r.checks()
    r.mount()
    r.test("read_cluster", "measure", directory)
    finish(r, "read_cluster", directory, "measure")


def checksums(r, checksum="crc32c"):
    r.format(checksum=checksum)
    r.mount()
    r.test("checksums", "create", r.path("test"))
    r.unmount()
    r.checks()
    r.host_test("checksums", "disk", r.image)
    r.mount()
    finish(r, "checksums", r.path("test"))


def xxhash_metadata(r):
    r.format(checksum="xxhash")
    journal = r.case_dir / "damage.json"
    for kind in ("super", "metadata"):
        for copies in ("one", "all"):
            r.host_test("xxhash", "damage", r.image, journal, kind, copies)
            if copies == "one":
                r.mount("ro")
                r.unmount()
            else:
                r.test("xxhash", "reject", r.args.device, r.mountpoint)
            r.host_test("xxhash", "repair", r.image, journal)
            r.checks()


def hardlink_limit(r):
    r.format(size="512M")
    r.mount()
    for name in ("first", "second"):
        r.test("hardlink", "limit", r.path(name))
    r.unmount()
    r.checks()
    for mode in ("ro", "rw"):
        r.mount(mode)
        for name in ("first", "second"):
            r.test("hardlink", "verify-limit", r.path(name))
        if mode == "rw":
            r.test("idle", None, r.path("idle"))
        r.unmount()
    r.checks()


def forced(r, script, phase):
    r.format()
    r.mount()
    if script == "devices":
        r.test("devices", "create", r.path("devices"))
    r.test(script, phase)
    r.checks()
    r.mount()
    r.test("idle", None, r.path("after-revoke"))
    r.unmount()
    r.checks()


def devices(r):
    r.format()
    r.mount()
    r.test("devices", "create", r.path("test"))
    r.test("devices", "race", r.path("races"))
    finish(r, "devices", r.path("test"), readonly="readonly")
    r.mount("nodev")
    r.test("devices", "nodev", r.path("test"))
    r.unmount()
    r.checks()


def devices_import(r):
    seed = r.case_dir / "devices-seed"
    r.host_test("run_fixtures", "device-seed", seed)
    r.format(seed, free_space="extent")
    r.host_test("run_fixtures", "devices", r.image)
    r.checks()
    r.mount()
    r.test("devices", "seed-verify")
    finish(r, "devices", verify="seed-verify")


def filehandle(r):
    r.format()
    saved = "/tmp/btrfs-run-filehandles.json"
    r.mount()
    r.test("filehandle", "exercise", r.path("test"), saved)
    r.unmount()
    r.test("filehandle", "stale", saved)
    r.checks()
    for mode, phase in (("ro", "readonly"), ("rw", "verify")):
        r.mount(mode)
        r.test("filehandle", "stale", saved)
        r.test("filehandle", phase, r.path("test"))
        r.unmount()
    r.checks()


def imported(r, script, seed_phase="seed", exercise="exercise",
             verify="verify", readonly=None, compress=None, restore=None,
             **format_args):
    seed = r.seed(script, seed_phase)
    r.format(seed, compress=compress, **format_args)
    r.mount()
    r.test(script, exercise)
    finish(r, script, verify=verify, readonly=readonly)
    if restore:
        r.restore(script, restore)


def compressed_reject(r, script, operation="reject"):
    seed_phase = "seed_compressed" if script == "grow" else "seed"
    seed = r.seed(script, seed_phase)
    r.format(seed, compress="zlib")
    r.mount()
    r.test(script, operation)
    r.unmount()
    r.checks()
    phase = ("verify-seed" if script == "inline" else
             "verify_compressed_seed" if operation == "reject" else
             "verify_compressed")
    r.restore(script, phase)
    r.mount("ro")
    r.unmount()


def shrink_compressed(r, codec):
    seed = r.seed("shrink_compressed")
    r.format(seed, compress=codec)
    r.mount()
    r.test("shrink_compressed", "exercise_zstd" if codec == "zstd" else "exercise")
    # Metadata checks work even when the kernel cannot decode retained data.
    finish(r, "shrink_compressed", verify="verify_metadata")
    r.restore("shrink_compressed", "verify")
    if codec == "zstd":
        r.mount("ro")
        r.test("shrink_compressed", "verify")
        r.unmount()


def lookup(r):
    seed = r.seed("lookup")
    r.format(seed, subvols=("rw:subvol",))
    r.mount()
    r.test("lookup", "verify")
    finish(r, "lookup")


def readdir(r):
    seed = r.seed("readdir")
    r.format(seed)
    r.host_test("readdir", "gaps", r.image)
    r.checks()
    r.mount()
    r.test("readdir", "exercise")
    finish(r, "readdir")


def policy_seed(r):
    seed = r.seed("nodatasum")
    flags = []
    for policy in ("nodatasum", "nodatacow"):
        for path in sorted((seed / policy).iterdir()):
            flags.append(f"{policy}:{path.relative_to(seed)}")
    return seed, flags


def nodatasum(r, capacity=False):
    seed, flags = policy_seed(r)
    r.format(seed, flags=flags, size=small_size(r) if capacity else None)
    r.mount()
    r.test("nodatasum", "capacity" if capacity else "exercise")
    r.unmount()
    r.checks()
    if not capacity:
        r.host_test("nodatasum", "disk", r.image)
        r.mount()
        finish(r, "nodatasum")
        r.restore("nodatasum", "verify-restored")


def inherit(r):
    seed = r.seed("inherit")
    r.format(seed, flags=("nodatacow:nocow", "nodatacow:xattr"))
    r.mount()
    r.test("inherit", "exercise")
    r.unmount()
    r.checks()
    r.host_test("inherit", "disk", r.image)
    r.mount()
    r.test("inherit", "more")
    finish(r, "inherit")


def xattrs(r):
    basic(r, "xattrs", readonly="readonly")
    r.host_test("xattrs", "disk", r.image)


def shrink_policy(r, policy):
    seed = r.seed("shrink")
    flags = [f"{policy}:{path.name}" for path in sorted(seed.iterdir())]
    r.format(seed, flags=flags)
    r.mount()
    r.test("shrink", "exercise")
    finish(r, "shrink", readonly="readonly")


def free_space(r, damage=None):
    seed = r.seed("free_space")
    # Keep the fixture within the rewriter's single-leaf constraint.
    r.format(seed, data="single", free_space="extent", size="256M")
    r.host_test("free_space", "bitmap", r.image)
    r.checks()
    if damage:
        r.host_test("free_space", damage, r.image)
        result = r.command(["btrfs", "check", "--readonly", r.image], check=False)
        assert result != 0, "checker accepted deliberately inconsistent free space"
        r.test("free_space", "reject", r.args.device, r.mountpoint)
        r.host_test("mirrors", r.image)
        return
    r.mount()
    r.test("free_space", "exercise")
    r.test("namespace", "create", r.path("namespace"))
    r.test("idle", None, r.path("idle"))
    r.test("statfs", "exercise", r.path("statfs"))
    r.unmount()
    r.checks()
    for mode in ("ro", "rw"):
        r.mount(mode)
        r.vm_python(
            "from pathlib import Path; import sys; "
            "assert Path(sys.argv[1]).read_bytes()==b'bitmap reuse\\n'",
            r.path("imported"))
        r.test("namespace", "verify", r.path("namespace"))
        r.unmount()
    r.checks()


def subvol_seed(r, directory=False):
    seed = r.seed("subvol")
    if directory:
        (seed / "left/nested/directory").mkdir()
    # Existing rejection fixtures use these IDs; verify mkfs's assignment.
    r.format(seed, subvols=SUBVOLS)
    tree = r.inspect("dump-tree", "-t", "root")
    for treeid in (256, 257, 258, 259):
        assert re.search(rf"key \({treeid} ROOT_ITEM ", tree), treeid
    return seed


def subvol(r):
    subvol_seed(r)
    r.vm("python3", r.vm_tests + "/subvol.py", "exercise", r.args.device,
         r.mountpoint + "-views", "256", "257", "258", "259")
    r.checks()
    r.mount()
    r.test("subvol", "verify")
    r.test("rename", "subvol")
    r.test("rename", "create", r.path("left/nested/rename"))
    r.unmount()
    r.checks()
    left, right = r.mountpoint + "-left", r.mountpoint + "-right"
    r.vm("mkdir", "-p", left, right)
    r.mount(point=left, tree=256)
    r.mount(point=right, tree=259)
    r.test("filehandle", "subvol", left, right)
    r.unmount(left)
    r.unmount(right)
    r.checks()
    r.mount()
    finish(r, "subvol")


def subvolume(r):
    r.format()
    r.mount()
    r.test("subvolume", "create")
    r.unmount()
    r.checks()
    for mode in ("ro", "rw"):
        r.mount(mode)
        r.test("subvolume", "verify")
        listing = r.vm("btrfs", "subvolume", "list", r.mountpoint, capture=True)
        latest = re.findall(r"^ID (\d+) top level 5 rw path latest$",
                            listing, re.MULTILINE)
        assert len(latest) == 1, listing
        r.unmount()
    r.checks()
    r.mount(tree=int(latest[0]))
    r.test("subvolume", "child")
    r.unmount()
    r.checks()
    r.mount()
    for phase in ("final", "boundaries", "collisions", "race", "abi"):
        r.test("subvolume", phase)
    finish(r, verify=None)


def subvolume_orphans(r):
    r.format()
    r.mount()
    r.test("subvolume", "orphans")
    r.unmount()
    r.checks()
    r.mount()
    r.test("subvolume", "orphan_verify")
    finish(r, verify=None)


def subvolume_capacity(r):
    # The fixed file count must exceed the complete deletion reservation.
    r.format(size="128M", nodesize=16384, data="single",
             free_space="extent", block_groups=False)
    r.mount()
    r.test("subvolume", "capacity")
    finish(r, verify=None)


def orphan_reject(r, directory=False):
    subvol_seed(r, directory)
    if directory:
        tree = r.inspect("dump-tree", "-t", "257")
        entries = [item for item in tree.split("\titem ")
                   if re.search(r"\bname: directory\n", item)]
        refs = [re.search(r"key \((\d+) INODE_REF \d+\)", item)
                for item in entries]
        inodes = [int(match[1]) for match in refs if match]
        assert len(inodes) == 1
        treeid, inode = 257, inodes[0]
    else:
        treeid, inode = 1, 259
    r.host_test("orphan", "inject", r.image, treeid, inode)
    r.checks()
    r.test("orphan", "reject", r.args.device, r.mountpoint)
    # Failed writable mount teardown must allow an unrelated clean filesystem.
    r.format()
    r.mount()
    r.test("idle", None, r.path("after-reject"))
    r.unmount()
    r.checks()


def chunks(r, phase="create", system=False, size=None):
    if size is None:
        size = "512M" if phase in ("create", "combined") else small_size(r)
    r.format(size=size, system=system)
    r.mount()
    r.test("chunks", phase, r.path("test"))
    verify = {"create": "verify", "combined": "combined_verify",
              "metadata": "metadata_verify", "capacity": None}[phase]
    finish(r, "chunks", r.path("test"), verify)


def chunks_large(r):
    # Keep the imported bitmaps within the fixture helper's single leaf.
    # Other layouts reach the 256 MiB cap; bitmaps exercise a scaled target.
    chunks(r, size="3G" if r.layout.free_space == "bitmap" else "8G")


def reclaim(r, bounded=True, bitmap_retire=False):
    r.format(size=small_size(r), free_space="extent" if bitmap_retire else None)
    directory = r.path("test")
    r.mount()
    r.test("reclaim_chunks", "prepare", directory)
    r.unmount()
    r.checks()
    before_data = r.case_dir / "data.json"
    before_metadata = r.case_dir / "metadata.json"
    r.host_test("reclaim_chunks", "snapshot", r.image, before_data)
    r.mount()
    r.test("reclaim_chunks", "free_data", directory)
    if bitmap_retire:
        r.unmount()
        r.host_test("free_space", "bitmap-groups", r.image)
        r.checks()
        r.mount()
    r.test("reclaim_chunks", "metadata_bounded" if bounded else "metadata", directory)
    r.unmount()
    r.checks()
    r.host_test("reclaim_chunks", "reassigned", r.image, before_data,
                "DATA", "METADATA")
    for mode in ("ro", "rw"):
        r.mount(mode)
        r.test("reclaim_chunks", "verify_metadata", directory)
        r.unmount()
    if bounded:
        r.host_test("reclaim_chunks", "snapshot", r.image, before_metadata)
        r.mount()
        r.test("reclaim_chunks", "free_metadata", directory)
        r.test("reclaim_chunks", "data_again", directory)
        r.unmount()
        r.checks()
        r.host_test("reclaim_chunks", "reassigned", r.image, before_metadata,
                    "METADATA", "DATA")
        r.mount()
        finish(r, "reclaim_chunks", directory, "verify_data")


def statfs(r):
    r.format(size=small_size(r))
    expected = r.host_test("statfs", "disk", r.image, capture=True).strip()
    saved = "/tmp/btrfs-run-statfs.json"
    r.vm_python("from pathlib import Path; import sys; "
                "Path(sys.argv[1]).write_text(sys.argv[2])", saved, expected)
    r.mount("ro")
    r.test("statfs", "check", r.mountpoint, saved)
    r.unmount()
    r.mount()
    # Exhaust physical growth first, then release a small, measured amount of
    # data. Older capacity tests assumed chunks could not grow at all.
    r.vm_python(
        "import os,errno,sys\n"
        "fd=os.open(sys.argv[1],os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)\n"
        "count=0\n"
        "while True:\n"
        " try: os.write(fd,b'P'*4096)\n"
        " except OSError as e:\n"
        "  assert e.errno==errno.ENOSPC; break\n"
        " count+=1\n"
        " if count%128==0: os.fsync(fd)\n"
        "os.fsync(fd)\n"
        "assert os.statvfs(sys.argv[1]).f_bavail==0\n"
        "assert count>2048\n"
        "os.ftruncate(fd,(count-2048)*4096)\n"
        "os.fsync(fd); os.close(fd)\n",
        r.path("physical-filler"))
    r.test("statfs", "exercise", r.path("test"))
    r.test("statfs", "capacity", r.path("test"))
    r.unmount()
    r.checks()
    expected = r.host_test("statfs", "disk", r.image, capture=True).strip()
    r.vm_python("from pathlib import Path; import sys; "
                "Path(sys.argv[1]).write_text(sys.argv[2])", saved, expected)
    for mode in ("ro", "rw"):
        r.mount(mode)
        r.test("statfs", "check", r.mountpoint, saved)
        r.test("statfs", "verify", r.path("test"))
        r.unmount()
    r.checks()


def capacity(r, script):
    seed = None
    if script in ("inline", "grow", "shrink", "zstd_cow", "holes"):
        seed = r.seed(script)
    if script == "orphan_cleanup":
        seed = r.case_dir / "seed"
        seed.mkdir()
        with (seed / "imported").open("wb") as output:
            for _ in range(64):
                output.write(b"I" * (1024 * 1024))
    r.format(seed, size=small_size(r),
             compress="zstd" if script == "zstd_cow" else None,
             holes=True if script == "holes" else None)
    r.mount()
    if script == "orphan_cleanup":
        r.vm("rm", r.path("imported"))
        r.vm("sync")
    imported_script = script in ("inline", "grow", "shrink", "zstd_cow", "holes")
    directory = r.mountpoint if imported_script else r.path("test")
    if script == "enospc":
        r.test(script, None, directory)
    else:
        r.test(script, "capacity", directory)
    verify = {"unlink": "capacity_verify",
              "truncate_batches": "capacity_verify"}.get(script)
    finish(r, script, directory, verify)


def inline_zstd_capacity(r):
    seed = r.seed("inline", "seed-zstd")
    r.format(seed, size=small_size(r), compress="zstd")
    r.mount()
    r.test("inline", "capacity-zstd")
    finish(r, verify=None)


def ready_recovery(r, script, nested=False, during_mount=False):
    if nested:
        subvol_seed(r)
    else:
        r.format(size="512M" if script == "chunks" else "256M")
    r.mount()
    directory = r.path("left/nested/recovery" if nested else "recovery")
    marker = {"orphan_cleanup": "/tmp/btrfs-orphan-ready",
              "rename": "/tmp/btrfs-rename-ready",
              "chunks": "/tmp/btrfs-chunks-ready"}[script]
    r.crash_ready(script, directory, marker)
    r.checks()
    r.boot()
    if during_mount:
        r.crash_break(["mount_btrfs", r.args.device, r.mountpoint],
                      ["btrfs_write_super_mirrors"], hits=3)
        r.checks()
        r.boot()
    if nested:
        # Mount a clean sibling first: recovery spans all file trees.
        r.mount(tree=259)
        r.unmount()
    r.mount()
    r.test(script, "verify" if script == "chunks" else "recovered", directory)
    r.unmount()
    r.checks()


def truncate_recovery(r, batches=False):
    r.format()
    directory = r.path("recovery")
    r.mount()
    r.test("truncate_batches", "prepare", directory)
    if batches:
        targets, hits = ["btrfs_cleanup_inode", "btrfs_write_super_mirrors"], 2
    else:
        targets, hits = ["btrfs_cleanup_inode"], 1
    r.crash_break(r.test_argv("truncate_batches", "truncate", directory),
                  targets, hits)
    r.checks()
    r.boot()
    r.mount()
    r.test("truncate_batches", "recovered", directory)
    finish(r, "truncate_batches", directory)


def publication(r, after=False):
    r.format()
    directory = r.path("recovery")
    r.mount()
    r.test("coalesce", "create", directory)
    target = "btrfs_space_publish_chunk" if after else "btrfs_write_super_mirrors"
    r.crash_break(r.test_argv("coalesce", "crash_write", directory), [target])
    r.checks()
    r.boot()
    r.mount()
    finish(r, "coalesce", directory, "verify_crash" if after else "verify_original")


def reclaim_recovery(r):
    r.format(size=small_size(r))
    directory = r.path("recovery")
    r.mount()
    r.test("reclaim_chunks", "prepare", directory)
    r.test("reclaim_chunks", "free_data", directory)
    r.crash_break(r.test_argv("reclaim_chunks", "metadata_bounded", directory),
                  ["btrfs_free_chunk_items", "btrfs_space_publish_chunk"])
    r.checks()
    r.boot()
    r.mount()
    r.test("idle", None, r.path("after-retirement"))
    r.unmount()
    r.checks()


def cases():
    result = {}
    for script in ("namespace", "symlink", "orphan_cleanup", "truncate_batches"):
        result[script] = partial(basic, script=script)
    for script in ("flags", "unlink", "rmdir", "rename", "ipc"):
        result[script] = partial(basic, script=script, readonly="readonly")
    result.update({
        "idle": partial(basic, script="idle", create=None, verify=None),
        "locks": partial(basic, script="locks", create=None, verify=None,
                         readonly="readonly"),
        "kqueue": partial(basic, script="kqueue", verify=None, readonly="readonly"),
        "locks-reclaim": partial(forced, script="locks", phase="reclaim"),
        "kqueue-revoke": partial(forced, script="kqueue", phase="revoke"),
        "devices": devices, "devices-import": devices_import,
        "devices-reclaim": partial(forced, script="devices", phase="reclaim"),
        "filehandle": filehandle,
        "hardlink": partial(basic, script="hardlink", create="create-extended",
                            readonly="readonly"),
        "hardlink-no-extref": partial(basic, script="hardlink", extref=False,
                                      readonly="readonly"),
        "hardlink-limit": hardlink_limit,
        "rename-races": partial(basic, script="rename", create="races", verify=None),
        "rename-packed": partial(basic, script="rename", create="packed",
                                  verify=None, extref=False),
        "checksums": checksums, "cluster": cluster, "coalesce": coalesce,
        "xxhash-checksums": partial(checksums, checksum="xxhash"),
        "xxhash-read-import": partial(read_import, checksum="xxhash"),
        "xxhash-read-compressed": partial(read_import, compressed=True,
                                          checksum="xxhash"),
        "xxhash-read-faults": partial(read_faults, checksum="xxhash"),
        "xxhash-metadata": xxhash_metadata,
        "reflink": reflink,
        "read-cluster": read_cluster,
        "read-range": partial(basic, script="read_range"),
        "write-batch-capacity": write_batch_capacity,
        "write-range": partial(basic, script="write_range"),
        "read-import": read_import,
        "read-compressed": partial(read_import, compressed=True),
        "read-faults": read_faults,
        "lookup": lookup, "readdir": readdir, "subvol": subvol,
        "subvolume": subvolume,
        "subvolume-orphans": subvolume_orphans,
        "subvolume-zstd": partial(imported, script="subvolume",
                                  exercise="compressed",
                                  verify="compressed_verify", compress="zstd",
                                  subvols=("rw:imported",)),
        "subvolume-capacity": subvolume_capacity,
        "inline": partial(imported, script="inline", exercise="write"),
        "inline-zstd": partial(imported, script="inline", seed_phase="seed-zstd",
                               exercise="write-zstd", verify="verify-zstd",
                               compress="zstd", restore="verify-restored-zstd"),
        "inline-zlib-reject": partial(compressed_reject, script="inline"),
        "grow": partial(imported, script="grow", readonly="readonly"),
        "grow-zlib-reject": partial(compressed_reject, script="grow"),
        "grow-zlib": partial(compressed_reject, script="grow", operation="compressed"),
        "grow-zstd": partial(imported, script="grow", seed_phase="seed_compressed",
                             exercise="zstd", verify="verify_zstd", compress="zstd"),
        "shrink": partial(imported, script="shrink", readonly="readonly"),
        "shrink-nodatasum": partial(shrink_policy, policy="nodatasum"),
        "shrink-nodatacow": partial(shrink_policy, policy="nodatacow"),
        "shrink-zlib": partial(shrink_compressed, codec="zlib"),
        "shrink-zstd": partial(shrink_compressed, codec="zstd"),
        "zstd-cow": partial(imported, script="zstd_cow", compress="zstd",
                            restore="restore"),
        "holes": partial(imported, script="holes", exercise="write", holes=True),
        "nodatasum": nodatasum, "inherit": inherit, "xattrs": xattrs,
        "free-space": free_space,
        "free-space-bad-count": partial(free_space, damage="bad-count"),
        "free-space-bad-bit": partial(free_space, damage="bad-bit"),
        "orphan-root-reject": orphan_reject,
        "orphan-directory-reject": partial(orphan_reject, directory=True),
        "statfs": statfs,
        "chunks": chunks,
        "chunks-large": chunks_large,
        "chunks-system": partial(chunks, system=True),
        "chunks-combined": partial(chunks, phase="combined"),
        "chunks-system-combined": partial(chunks, phase="combined", system=True),
        "chunks-metadata": partial(chunks, phase="metadata"),
        "chunks-capacity": partial(chunks, phase="capacity"),
        "reclaim": reclaim,
        "reclaim-full": partial(reclaim, bounded=False),
        "reclaim-bitmap-retire": partial(reclaim, bitmap_retire=True),
        "nodatasum-capacity": partial(nodatasum, capacity=True),
        "inline-zstd-capacity": inline_zstd_capacity,
        "recover-orphan": partial(ready_recovery, script="orphan_cleanup"),
        "recover-orphan-nested": partial(ready_recovery, script="orphan_cleanup",
                                         nested=True),
        "recover-orphan-mount": partial(ready_recovery, script="orphan_cleanup",
                                        during_mount=True),
        "recover-rename": partial(ready_recovery, script="rename"),
        "recover-chunks": partial(ready_recovery, script="chunks"),
        "recover-truncate": truncate_recovery,
        "recover-truncate-batches": partial(truncate_recovery, batches=True),
        "publish-before": publication,
        "publish-after": partial(publication, after=True),
        "recover-reclaim": reclaim_recovery,
    })
    for script in ("inline", "grow", "shrink", "zstd_cow", "holes", "unlink",
                   "rmdir", "rename", "orphan_cleanup", "truncate_batches", "enospc"):
        result[script.replace("_", "-") + "-capacity"] = partial(capacity, script=script)
    return result
