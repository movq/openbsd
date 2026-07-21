# Transient softraid AES-XTS crypto mode

## Purpose

Add a runtime-configured softraid discipline that exposes an encrypted block
device without requiring a softraid header, key metadata, or reserved metadata
area on the backing device.

The intended use is mapping an existing encrypted payload whose geometry and
raw AES-XTS key have already been determined by userland, including VeraCrypt
volumes using Linux-compatible `aes-xts-plain64` sector tweaks.

The mode should reuse softraid's established:

- SCSI command admission and completion
- work-unit and CCB pools
- asynchronous child-buffer I/O
- DMA-safe write bounce buffers
- serialized crypto taskq
- device attachment and detachment
- lower-device error propagation

It should not add another asynchronous engine to `vnd(4)`.

## Non-goals

The first implementation does not provide:

- an on-disk softraid format
- metadata probing or auto-assembly
- passphrase derivation in the kernel
- encrypted or wrapped key storage
- key disks or passphrase changes
- boot or hibernation support
- rebuild, redundancy, or hot-spare support
- regular-file backing
- compatibility with native softraid CRYPTO metadata

Userland remains responsible for parsing the container format, deriving or
recovering the raw XTS key, validating format-specific parameters, and
destroying its copy of the key after configuration.

## High-level approach

Introduce a distinct discipline, tentatively `SR_MD_CRYPTOPLAIN`, rather than
making native `SR_MD_CRYPTO` metadata optional through conditionals spread
throughout its create, assemble, and metadata paths.

`SR_MD_CRYPTOPLAIN` should share narrowly factored crypto and lower-I/O helpers
with `SR_MD_CRYPTO`, but have its own configuration and lifecycle entry
points. This keeps the native softraid format unchanged and makes accidental
metadata writes by the transient mode easier to exclude and assert against.

The resulting mapping attaches through softraid's existing SCSI bus and
appears as an `sd(4)` disk. Disklabel handling therefore remains in the normal
SCSI disk stack rather than becoming part of the crypto mapper.

## Runtime configuration

The configuration supplied by privileged userland needs:

```c
struct sr_crypto_plain_config {
	dev_t		 backing_dev;
	uint64_t	 data_offset;
	uint64_t	 data_length;
	uint64_t	 iv_offset;
	uint32_t	 secsize;
	uint32_t	 xts_secsize;
	uint32_t	 flags;
	uint8_t		 key[64];
};
```

The exact ABI can differ. Required semantics are:

- `backing_dev` names one block-device partition
- `data_offset` and `data_length` are bytes relative to that partition
- `secsize` is the sector size exposed by the new `sd(4)` device
- `xts_secsize` is the XTS data-unit size
- `iv_offset` is added to the first virtual XTS data-unit number
- `key` is a 512-bit AES-XTS key, containing two 256-bit AES keys
- a flag may make the mapping read-only

The ioctl must copy the key into kernel-owned memory and clear the ioctl
buffer before returning. Configuration failure and deletion must explicitly
clear every kernel copy of the raw key and provider key schedule.

Do not overload native `BIOCCREATERAID` in a way that can fall through to
metadata probing, clearing, creation, or saving. Use either a dedicated ioctl
or an explicitly separate branch entered before the normal metadata path.

## Geometry validation

Configuration must validate all geometry before opening the mapping:

- data length is non-zero
- all sector sizes are supported powers of two
- `xts_secsize <= secsize`
- `secsize` is a multiple of `xts_secsize`
- data offset and length do not overflow or exceed the backing partition
- data length is a multiple of the exposed sector size
- the final `plain64` data-unit number does not overflow `uint64_t`
- read-write configuration requires a writable backing device

Direct child I/O must also satisfy the backing device's logical-sector
alignment. Obtain its disklabel sector size during configuration and require:

- `data_offset` is backing-sector aligned
- `secsize` is a multiple of the backing sector size

These restrictions avoid read-modify-write and overlapping-write locking.
Reject incompatible mappings rather than silently changing their data path.

The backing range should be expressed internally in 64-bit byte or DEV_BSIZE
units. Do not reuse the 32-bit `ssd_data_blkno` field as the authoritative
offset if that would limit otherwise valid container offsets.

## Synthetic in-memory state

Generic softraid and SCSI helpers currently read geometry and identity through
`struct sr_metadata` and chunk structures. The transient create path may
allocate and populate synthetic in-memory instances containing only the
fields required by those helpers:

- one online chunk
- exposed volume size and sector size
- one backing vnode and device number
- an in-memory data offset
- an ephemeral volume identity and display name
- online volume and chunk status

Synthetic metadata must never be passed to a metadata writer. Add an explicit
discipline or metadata flag such as `SR_META_F_TRANSIENT`, and make attempts
to probe, clear, save, roam, or auto-assemble it fail or assert in diagnostic
kernels.

The transient discipline should not create crypto optional-metadata objects.
Its raw key, IV offset, XTS data-unit size, session ID, and data offset belong
in a runtime-only discipline structure.

## Backing-device ownership

Native softraid normally operates on dedicated `FS_RAID` partitions and
submits direct child buffers. A transient mapping over an arbitrary block
partition needs an equally clear ownership rule.

At minimum:

- reject a backing device already used by another softraid discipline
- reject a mounted or otherwise exclusively claimed partition
- flush and invalidate existing block-vnode buffers before direct I/O begins
- prevent concurrent buffered access to the backing partition while mapped
- keep the backing vnode open until all work and task callbacks are drained

If the kernel cannot enforce the no-alias rule reliably, restrict the initial
interface to a dedicated partition type or another ownership mechanism that
can enforce it. A documented userland convention alone is not sufficient to
prevent stale dirty buffers from overwriting direct ciphertext writes.

## Crypto data path

Factor descriptor preparation so native and transient crypto can supply:

- raw key and session ID
- XTS data-unit size
- first tweak number
- request buffer and length

For a transient request:

```text
virtual_byte =
    SCSI logical block number * exposed sector size

backing_byte =
    data_offset + virtual_byte

plain64_sector =
    iv_offset + virtual_byte / XTS data-unit size
```

Create one crypto descriptor per XTS data unit. The descriptor IV handling
must preserve the crypto framework's provider-compatible sector-number
convention on both little- and big-endian systems. Test provider migration as
well as the normal provider; do not assume that writing an `htole64()` value
into `crd_iv` matches every current provider.

The first version should use one crypto session and one non-MPSAFE taskq
worker. Existing software and AES-NI XTS sessions contain mutable operation
state and must not be invoked concurrently through one session.

Unlike native softraid CRYPTO, transient mode uses one supplied key across the
complete mapping. It must not select a new key every 0.5 TB.

## Read and write flow

Retain the existing softraid work-unit model:

- preallocate work units, CCBs, crypto requests, and DMA-safe bounce buffers
- copy plaintext writes into a work-unit bounce buffer
- encrypt writes before lower submission
- submit encrypted writes with `sr_ccb_rw()` or a factored equivalent
- read ciphertext directly into the upper SCSI buffer
- decrypt successful reads before completing the SCSI transfer
- return the work unit only after upper completion is committed

Normalize lower errors carefully:

- never decrypt a failed or short read as though it were complete
- never report ciphertext as a successful read prefix
- do not report a successful write prefix that ends inside an XTS data unit
- preserve a meaningful lower error rather than reducing every failure to an
  undifferentiated success or zero residual
- complete an upper request exactly once after a crypto failure

The existing native crypto paths require an audit here before they are shared.
In particular, `sr_crypto_rw()` must not continue to lower submission after
completing a request for an encryption error.

## Flush and discard

SCSI `SYNCHRONIZE CACHE` must provide a real persistence barrier:

1. stop later work from passing the synchronization command
2. wait for all older crypto and child writes to complete
3. issue `DIOCCACHESYNC` to the backing block device
4. propagate a lower flush failure to the SCSI command
5. allow later work to proceed only after the flush returns

The current generic softraid sync path drains pending work but does not itself
forward a lower cache flush. Add a transient-specific sync method or improve
the shared helper without changing native semantics unintentionally.

Discard is optional for the first implementation. If enabled, it must:

- drain older work and prevent later work from overtaking the discard
- validate ranges against the exposed volume
- translate ranges by the data offset
- preserve sector alignment and overflow checks
- reject discard on a read-only mapping

Discard reveals allocation patterns and destroys ciphertext without knowing
the plaintext format. Userland should opt into it explicitly if that policy is
not already implied by the mapping interface.

## Creation sequence

A dedicated transient create path should:

1. copy and clear the user-supplied key material
2. validate the complete configuration
3. acquire and validate exclusive ownership of the backing block device
4. flush and invalidate backing vnode buffers
5. allocate the discipline, transient runtime state, and synthetic geometry
6. create one online chunk without probing or reading softraid metadata
7. initialize the existing one-thread discipline taskq
8. allocate work units, CCBs, crypto requests, and DMA bounce buffers
9. create the single AES-XTS session from the raw key
10. attach the discipline as a normal softraid SCSI target
11. publish the mapping only after every resource is initialized
12. clear temporary key material on both success and failure

Failure unwind must be valid after every step and run in exact reverse
ownership order. No metadata sector may be read, cleared, or written merely
because transient creation failed.

## Deletion and shutdown

Deletion should use the established SCSI and softraid shutdown mechanisms:

1. stop new SCSI admission
2. drain outstanding work units and child buffers
3. flush a writable backing device
4. abort deletion and leave the mapping usable if the flush fails, unless a
   separate force-delete operation was explicitly requested
5. detach the SCSI LUN
6. drain and destroy the discipline taskq
7. free crypto requests and clear DMA bounce buffers
8. free the crypto session and explicitly clear raw keys
9. close and release the backing vnode
10. free synthetic geometry and discipline state

Transient deletion must never save or clear softraid metadata.

## Native softraid issues to resolve

Reusing the softraid pipeline reduces new concurrency code, but it does not
make all inherited behavior automatically correct. Before enabling transient
mode, audit and resolve at least:

- encryption failure followed by continued lower submission
- `crypto_invoke()` session migration and `EAGAIN`
- synchronization that drains but does not flush the lower device cache
- read and write residual handling on short lower transfers
- XTS IV behavior across crypto providers and big-endian systems
- work-unit and taskq draining during forced and normal detach
- direct-I/O backing ownership and buffer-cache aliases
- bounds checks at the final exposed sector
- cleanup after partially allocated crypto work units

Prefer fixes in shared helpers only when native CRYPTO can use them without an
on-disk compatibility change. Otherwise keep transient-specific behavior
separate.

## Userland

`cryptctl(8)` or a new tool can retain responsibility for:

- parsing VeraCrypt or LUKS metadata
- prompting for credentials
- deriving and validating candidate keys
- choosing data offset, length, sector sizes, and IV offset
- invoking the transient configuration ioctl
- clearing all key material before exit

The kernel ABI should describe a generic raw AES-XTS plain64 mapping, not
VeraCrypt-specific headers. This keeps format parsing out of the kernel and
permits future LUKS support without changing the data path.

## Implementation stages

1. Add a transient discipline type and a creation stub that cannot touch
   metadata.
2. Build synthetic in-memory geometry and attach a read-only identity target.
3. Factor native crypto descriptor preparation without changing its output.
4. Add one-key plain64 preparation with configurable XTS data-unit and IV
   offsets.
5. Reuse the existing work-unit, CCB, and child-buffer path.
6. Correct crypto-error and short-I/O completion behavior.
7. Implement and test a real lower cache flush barrier.
8. Add read-only transient creation and deletion.
9. Add writable mappings after backing ownership and flush behavior are
   demonstrated.
10. Add optional discard only after ordering tests exist.

Keep the synchronous XTS `vnd(4)` implementation as a compatibility oracle
during development. Do not remove it until sector-for-sector tests show that
the transient softraid mapping produces identical ciphertext.

## Correctness tests

At minimum, test:

- sector-for-sector VeraCrypt and Linux `aes-xts-plain64` fixtures
- non-zero data and IV offsets
- supported exposed and XTS sector-size combinations
- first, last, and both sides of the `2^32` sector boundary
- mappings larger than the native softraid 0.5-TB key interval
- queue saturation beyond the number of work units
- mixed concurrent reads and writes
- lower read, write, short-transfer, crypto, and flush failures
- immediate lower completion
- repeated create, I/O, flush, and delete cycles
- deletion while I/O is active
- rejection of unaligned backing geometry
- rejection of mounted, aliased, or already claimed backing devices
- confirmation that metadata-area bytes are never modified
- provider migration and big-endian IV behavior where available

Use `DIAGNOSTIC`, `WITNESS`, memory poisoning, and deterministic lower-I/O
fault injection. Ordering tests must record lower submission, completion, and
flush events; filesystem stress alone is not proof of a correct barrier.

## Completion criteria

The first implementation is complete when:

- transient creation performs no softraid metadata I/O
- the mapping attaches as a normal `sd(4)` device
- all valid data I/O uses the established softraid work-unit path
- ciphertext matches the synchronous `vnd(4)` reference exactly
- flush reaches the backing device after all older writes and before later
  writes
- no failed or short read exposes ciphertext as successful plaintext
- no crypto failure submits or completes an upper request twice
- teardown cannot free a key, session, vnode, CCB, or work unit still in use
- backing buffer-cache aliases cannot overwrite direct ciphertext I/O
- native softraid CRYPTO on-disk behavior remains unchanged
