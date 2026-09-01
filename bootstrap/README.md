# Cross-building OpenBSD

These scripts build OpenBSD/amd64 on FreeBSD, Linux, or NetBSD without
modifying the host system. In this directory, **host** always means the
machine running the build and **target** always means the OpenBSD system being
produced.

## Complete build

Run the stages individually:

```sh
./bootstrap/bootstrap-bmake.sh
./bootstrap/build-tools.sh
./bootstrap/cross-build-userland.sh
./bootstrap/cross-build-kernel.sh
```

Or run the same sequence through the driver:

```sh
./bootstrap/cross-build.sh
```

The build stages userland and produces the kernel and amd64 bootloaders.
Creating a disk image or installation media is deliberately out of scope.

## Output paths

`OBJDIR` is the common output root and defaults to `bootstrap/obj`:

```text
bootstrap/obj/host/                 host LLVM and other host tools
bootstrap/obj/target-amd64/         OpenBSD target objects
bootstrap/obj/dest-amd64/           staged OpenBSD target userland
bootstrap/obj/target-tools-amd64/   generated cross-build wrappers
bootstrap/obj/stand-amd64/          collected bootloaders
```

`bootstrap/tools/` contains host executables shared by the later stages.
Set a custom output root consistently for every stage:

```sh
OBJDIR=/fast/openbsd-obj ./bootstrap/cross-build.sh
```

`HOST_OBJDIR`, `TARGET_OBJDIR`, `DESTDIR`, and `TARGET_TOOLDIR` can override
individual locations. The older stage-local `OBJROOT` variable remains
accepted for compatibility.

## Configuration

The commonly useful environment variables are:

```text
MACHINE, MACHINE_ARCH, MACHINE_CPU  target architecture (default: amd64)
TARGET_CANON                        clang target triple
KERNEL_CONFIG                       kernel configuration (default: GENERIC.MP)
HOST_CC, HOST_CXX                   host compilers
JOBS                                parallel make jobs
TOOLDIR                             installed host tools
BUILD_LLDB                          build target LLDB (default: no)
```

OpenBSD make variables such as `BUILD_CLANG` and `BUILDUSER` retain their
upstream names when passed into the source tree; they are not bootstrap
host/target terminology.

## Host support

Host-specific declarations, compatibility implementations, and settings live
under `bootstrap/platform/`. Linux uses libbsd in addition to its compatibility
sources. FreeBSD and NetBSD use their native BSD libc interfaces and supply
only the OpenBSD interfaces they lack.

The host needs a C/C++ compiler, flex, Perl, pax, and GNU-compatible
`objcopy`/`objdump` utilities. FreeBSD additionally needs bash and GNU binutils,
normally installed under `/usr/local/bin`. Linux additionally needs clang and
libbsd development headers for the existing Linux bootstrap path. The host tool
stage builds the OpenBSD clang/lld used for all target compilation.
