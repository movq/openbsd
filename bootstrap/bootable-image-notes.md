# Bootable OpenBSD Image Notes From GNU/Linux

These notes summarize what we found while trying to turn the Linux-built
`bootstrap/dest` tree into a bootable amd64 OpenBSD disk image.

## EFI Bootloader

For amd64 UEFI boot, install the OpenBSD EFI loader on the FAT EFI System
Partition at the removable/default path:

```text
/EFI/BOOT/BOOTX64.EFI
```

No additional loader support files are required on the ESP for the normal
case. The loader is self-contained: it includes OpenBSD's normal `boot(8)`
logic plus EFI disk probing and filesystem support.

The default path above is enough for firmware to start the loader. Once
running, the loader:

1. Probes EFI block devices.
2. Prefers the disk it was loaded from.
3. Searches the disk's GPT for an OpenBSD partition.
4. Reads the BSD disklabel inside that OpenBSD partition.
5. Boots the default kernel path, normally `hd0a:/bsd`.

Optional loader configuration lives at `/etc/boot.conf` on the OpenBSD root
filesystem, not on the ESP. For example:

```text
set timeout 1
# boot hd0a:/bsd
```

The EFI loader also has an `esp:` pseudo-device and can read from the EFI
System Partition, but the default root/kernel path still expects the OpenBSD
partition and disklabel layout.

## GPT And Disklabel

A GPT partition is fine, but it is not enough by itself. The amd64 EFI loader
finds the GPT OpenBSD partition and then reads a BSD disklabel from sector 1
inside that partition. A bare UFS/FFS filesystem written directly to a GPT
partition without a BSD disklabel is likely not bootable by the stock loader.

Use these GPT partition types:

```text
EFI System Partition: c12a7328-f81f-11d2-ba4b-00a0c93ec93b
OpenBSD Area:         824cc7a0-36a8-11e3-890a-952519ad3f61
```

OpenBSD's source represents the EFI System Partition UUID in on-disk byte
order as `28732ac1-1ff8-d211-ba4b-00a0c93ec93b`, but Linux GPT tools usually
display the canonical form above.

The Linux-host `makefs` built by `bootstrap/build-tools.sh` can write a simple
BSD disklabel into an FFS image with `rdroot=1`. This is currently the easiest
way to get a loader-readable OpenBSD root image without needing OpenBSD
`disklabel(8)` on the host:

```sh
bootstrap/tools/bin/makefs -t ffs -s 2g \
  -o rdroot=1,minfree=0,bsize=16384,fsize=2048,density=8192 \
  openbsd-root.ffs bootstrap/dest
```

The `rdroot=1` option writes an OpenBSD disklabel at sector 1 and creates an
`a` partition covering the filesystem. It also gives the label a DUID, which
the EFI loader passes to the kernel.

When the FFS image is written into a GPT OpenBSD partition, the `rdroot=1`
label still needs adjustment. The amd64 EFI loader reads the label from sector
1 inside the GPT OpenBSD partition, but partition offsets in the BSD disklabel
are interpreted as absolute disk LBAs. A plain `rdroot=1` label has
`a.offset=0`, which makes the loader try to read the root filesystem from the
protective MBR/GPT area. The bootstrap image script patches the label after
`makefs` so partition `a` starts at the OpenBSD GPT partition's absolute LBA,
sets a normal disk type, updates raw partition `c`, and recomputes the label
checksum. It also writes the OpenBSD GPT partition bounds into the label and
uses a controlled root DUID so `/etc/fstab` can refer to the root filesystem as
`<duid>.a`.

If the kernel panics with:

```text
panic: root device (<duid>) not found
```

the EFI loader did read a BSD disklabel and passed that label's DUID to the
kernel, but the kernel did not find any attached disk with a matching label.
That happens before `/etc/fstab` is used. Check the kernel boot messages for
the disk device (`sd0`, `wd0`, `vioblk0`, etc.) and for disklabel read errors.
The on-image label can be checked by reading sector `OpenBSD-GPT-start + 1`;
the DUID there should match the panic value.

One subtle case is a label that the EFI loader accepts but the kernel rejects.
The loader only checks magic and checksum, but the kernel also rejects labels
where fields such as `d_nsectors` or `d_secpercyl` are zero. The image script
therefore patches `makefs rdroot=1` labels to include non-zero synthetic
geometry.

The named `disklabel=...` makefs option is not useful in the current host
tool build because `bootstrap/compat/getdiskbyname.c` is a stub.

## Kernel And Loader Files

For a direct boot, the OpenBSD root filesystem needs a kernel at `/bsd`.
Based on the current bootstrap output, the kernel exists here:

```text
bootstrap/obj-host/sys/arch/amd64/compile/GENERIC.MP/bsd
```

Copy it into the image staging tree:

```sh
cp bootstrap/obj-host/sys/arch/amd64/compile/GENERIC.MP/bsd bootstrap/dest/bsd
chmod 644 bootstrap/dest/bsd
```

The EFI loader should be copied to the ESP. The bootstrap kernel script intends
to collect stand outputs into:

```text
bootstrap/obj-kernel/stand/BOOTX64.EFI
```

If that file is missing, either rerun/fix the stand-loader collection step or
copy `BOOTX64.EFI` from the stand object directory where `efiboot/bootx64`
actually built it.

## Populating `/etc`, `/root`, And `/var`

The native OpenBSD target for the configuration tree is:

```sh
make -C etc DESTDIR=... distribution-etc-root-var
```

That target does the right thing, but running it unmodified on GNU/Linux is not
trivial. It assumes OpenBSD host tools, OpenBSD group names, OpenBSD `install`,
`mtree`, `pwd_mkdb`, and OpenBSD filesystem semantics.

There are two possible approaches:

1. Port enough host tools and environment so `distribution-etc-root-var` works.
2. Recreate the useful parts of `distribution-etc-root-var` in a bootstrap
   script.

The path of least resistance is probably a hybrid:

1. Reuse the authoritative file lists and copy logic from `etc/Makefile`.
2. Avoid trying to run the whole target verbatim at first.
3. Build or port only the few tools that are truly needed, especially
   `pwd_mkdb`.
4. Let `makefs` set final ownership/modes from the staged tree where possible.

The current bootstrap helper for the copy/touch/link parts is:

```sh
bootstrap/populate-distribution.sh
```

It stages the main `/etc`, `/root`, `/var`, `/dev/MAKEDEV`, `rc.d`, example,
SSH, mail, TLS, LDAP, npppd, bgplg, and mtree files into `bootstrap/dest`.
It also creates a default `/etc/fstab` and `/etc/myname` if they do not
already exist.  It does not yet generate `/etc/pwd.db` or `/etc/spwd.db`;
those still require a host-capable `pwd_mkdb` or an OpenBSD-side generation
step.

Important pieces from `etc/Makefile`:

```text
distrib-dirs:
  creates directories from etc/mtree/4.4BSD.dist

distribution-etc-root-var:
  installs /etc files, /root dotfiles, skeleton files, rc.d scripts,
  mail config, signify keys, empty log/db files, and sysmerge metadata
```

For a first bootable system, at minimum make sure these exist:

```text
/etc/fstab
/etc/group
/etc/master.passwd
/etc/passwd
/etc/pwd.db
/etc/spwd.db
/etc/rc
/etc/rc.conf
/etc/ttys
/etc/myname or a plan to set hostname later
/dev/MAKEDEV
/root/.profile
/var/run
/var/db
/var/log
/var/tmp -> ../tmp
```

Example minimal `fstab` for the common first disk case:

```text
/dev/wd0a / ffs rw 1 1
```

Depending on the virtual hardware, root may be `sd0a` instead of `wd0a`.
The loader will still load `/bsd`; the kernel's root selection depends on the
boot arguments and disk attachment order.

### `pwd_mkdb`

The password databases are important. OpenBSD userland expects `passwd`,
`pwd.db`, and `spwd.db`, not just `master.passwd`.

The cross-built `pwd_mkdb` in `bootstrap/obj-host/usr.sbin/pwd_mkdb/pwd_mkdb`
is an OpenBSD binary and cannot run on Linux. Options:

1. Port/build a Linux-host `pwd_mkdb` with compatibility shims.
2. Generate the databases on a running OpenBSD system.
3. Boot a ramdisk/install environment first and run `pwd_mkdb` there.

Porting `pwd_mkdb` is probably less work than fully reproducing every detail
of `distribution-etc-root-var`, but it is still not currently available as a
Linux host tool in `bootstrap/build-tools.sh`.

## `/dev` Nodes

This is the trickiest part of direct multi-user boot from a Linux-created
root filesystem.

OpenBSD normally installs `/dev/MAKEDEV` and then creates device nodes by
running:

```sh
cd /dev && sh MAKEDEV all
```

The ramdisk images do this during image construction:

```text
SCRIPT  ${DESTDIR}/dev/MAKEDEV  dev/MAKEDEV
SPECIAL cd dev; sh MAKEDEV ramdisk
```

On a Linux staging tree, running OpenBSD `MAKEDEV` directly is not enough
unless the resulting special nodes have OpenBSD-compatible device numbers in
the UFS image.

### Linux `mknod` Caveat

The host `makefs` walks the staging tree with Linux `stat(2)` and copies
`st_rdev` directly into the UFS inode:

```c
dinp->di_rdev = cur->inode->st.st_rdev;
```

OpenBSD encodes `dev_t` as:

```c
major = (dev >> 8) & 0xff
minor = (dev & 0xff) | ((dev & 0xffff0000) >> 8)
dev   = ((major & 0xff) << 8) |
        (minor & 0xff) |
        ((minor & 0xffff00) << 8)
```

Linux uses a different `dev_t` encoding. Therefore, using Linux `mknod` with
OpenBSD major/minor numbers is only automatically safe for device nodes whose
major is small and whose minor is below 256. In that common low-minor case,
Linux's raw `st_rdev` happens to match OpenBSD's encoding.

That covers many critical early nodes, such as:

```text
/dev/console  c 0 0
/dev/tty      c 1 0
/dev/null     c 2 2
/dev/zero     c 2 12
/dev/stdin    c 22 0
/dev/stdout   c 22 1
/dev/stderr   c 22 2
```

It may not cover every node created by `MAKEDEV all`, especially devices with
minor numbers above 255.

### Recommended `/dev` Strategy

For a first direct boot:

1. Create only the minimal nodes needed to reach single-user or early
   multi-user boot.
2. Use Linux `mknod` for low-minor nodes where the raw encoding matches.
3. Once OpenBSD boots, run `cd /dev && sh MAKEDEV all` on OpenBSD to recreate
   the complete `/dev` tree correctly.

For robust image generation:

1. Patch the Linux-host `makefs` to translate Linux-decoded major/minor numbers
   into OpenBSD `dev_t` before writing `di_rdev`.
2. Then Linux `mknod` can be used with OpenBSD major/minor numbers for all
   nodes.

A makefs-side helper would look conceptually like:

```c
static uint32_t
openbsd_makedev(unsigned int major, unsigned int minor)
{
	return (((major & 0xff) << 8) |
	    (minor & 0xff) |
	    ((minor & 0xffff00) << 8));
}
```

Then device inodes should write:

```c
dinp->di_rdev = openbsd_makedev(major(st.st_rdev), minor(st.st_rdev));
```

The host build would also need the appropriate Linux `major()`/`minor()`
definitions, usually from `<sys/sysmacros.h>`.

## Suggested Build Flow

An initial end-to-end flow could be:

1. Build userland into `bootstrap/dest`.
2. Populate `bootstrap/dest` with the `/etc`, `/root`, and `/var` contents
   derived from `etc/Makefile`.
3. Generate password databases with a Linux-host `pwd_mkdb` or defer that step
   to an OpenBSD ramdisk/first boot.
4. Copy the kernel to `bootstrap/dest/bsd`.
5. Add a minimal `/etc/fstab`.
6. Add minimal `/dev` nodes, or patch `makefs` and create the full set.
7. Create an FFS image with `makefs -o rdroot=1,...`.
8. Create a disk image with GPT, ESP, and OpenBSD Area partitions.
9. Format/populate the ESP with `EFI/BOOT/BOOTX64.EFI`.
10. Write the FFS image into the OpenBSD GPT partition.

For first successful bootstrapping, consider booting into single-user mode
with a `boot.conf` line like:

```text
boot hd0a:/bsd -s
```

Then repair in-place from OpenBSD:

```sh
mount -uw /
pwd_mkdb -p /etc/master.passwd
cd /dev && sh MAKEDEV all
```

After that, remove the `-s` boot override and try a normal multi-user boot.
