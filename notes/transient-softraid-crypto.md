# Transient softraid AES-XTS crypto mode

## Purpose

Add a runtime-configured softraid CRYPTO mapping that exposes an encrypted
block device without requiring a softraid header, key metadata, or reserved
metadata area on the backing device.

The intended use is mapping an existing encrypted payload whose offset,
length, IV offset, and raw AES-XTS key have already been determined by
userland, including LUKS or VeraCrypt payloads using Linux-compatible
`aes-xts-plain64` sector tweaks.

The primary goal is the smallest implementation that works and has the same
I/O path and performance as native softraid CRYPTO. The transient mode should
inherit native CRYPTO's behavior, including its existing limitations and error
handling, rather than auditing or redesigning that behavior as part of this
work.

Userland is responsible for parsing the container format, deriving or
recovering the raw XTS key, validating format-specific parameters, and
destroying its copy of the key after configuration.

## High-level approach

Create a normal in-memory `SR_MD_CRYPTO` discipline by calling
`sr_crypto_discipline_init()`, then mark it transient. Populate enough
synthetic metadata and chunk state for the generic softraid and SCSI code to
operate, but never probe, read, clear, or save softraid metadata for this
mapping.

The transient flag changes only the parts that cannot be represented by
native metadata:

- the raw key is already available and does not need to be unmasked
- one key and one crypto session cover the complete mapping
- `iv_offset` is added to each virtual 512-byte block number
- metadata writes are skipped

All normal I/O should continue through the existing:

- SCSI command handling
- work-unit and CCB pools
- collision scheduling
- DMA-safe write bounce buffers
- `sr_crypto_prepare()`
- synchronous `crypto_invoke()`
- `sr_crypto_dev_rw()`
- lower `VOP_STRATEGY()` submission
- read completion and decryption
- SCSI attachment and detachment

The resulting mapping appears as a normal `sd(4)` disk. Disklabel handling
remains in the SCSI disk stack.

## Initial scope

Keep the first version deliberately narrow:

- AES-XTS with a 512-bit key, containing two 256-bit AES keys
- 512-byte exposed sectors
- 512-byte XTS data units
- one supplied key across the complete mapping
- one crypto session
- read-write mappings only
- no discard forwarding
- little-endian machines, where native CRYPTO's host-endian block-number IV
  is compatible with `plain64`
- data offsets representable by the existing 32-bit `ssd_data_blkno`
- the same backing-device ownership and shutdown behavior as native softraid

Supporting other sector sizes, read-only mappings, larger data offsets,
discard, stronger backing-device exclusion, or big-endian `plain64` is
explicitly deferred.

## Runtime configuration

Add a dedicated privileged ioctl with a fixed-size configuration structure.
For example:

```c
struct sr_crypto_plain_config {
	dev_t		 backing_dev;
	uint32_t	 flags;
	uint64_t	 data_offset;
	uint64_t	 data_length;
	uint64_t	 iv_offset;
	uint8_t		 key[64];
};
```

The exact ABI can differ. Initial semantics are:

- `backing_dev` names one block-device partition
- `data_offset` and `data_length` are bytes relative to that partition
- `iv_offset` is added to the virtual 512-byte block number
- `key` is passed in the same data-key/tweak-key order expected by
  `CRYPTO_AES_XTS`
- `flags` must be zero in the first version

Use a dedicated ioctl rather than `BIOCCREATERAID`. The normal create path
unconditionally enters metadata probing, attachment, optional clearing, and
saving. The transient create path must not be able to fall through to any of
those operations.

The ioctl should copy the key into the discipline's existing
`scr_key[0]` storage and clear the ioctl key field before returning. Temporary
kernel copies must be cleared on both success and failure. Existing session
destruction and final `explicit_bzero()` of the discipline provide the normal
teardown behavior for the retained kernel key and provider schedule.

## Geometry validation

Validate only what is needed by the existing 512-byte data path:

- `data_length` is non-zero
- `data_offset` and `data_length` are multiples of `DEV_BSIZE`
- `data_offset + data_length` does not overflow
- the range is contained within the backing partition
- the backing device reports a 512-byte sector size
- `data_offset / DEV_BSIZE` fits in `uint32_t`
- `data_length / DEV_BSIZE` fits in the existing signed volume-size field
- `iv_offset + (data_length / DEV_BSIZE) - 1` does not overflow `uint64_t`

Store the backing offset in synthetic `ssd_data_blkno` and the exposed size in
synthetic `ssdi.ssd_size`. This intentionally accepts the existing 32-bit
offset limit rather than adding a second lower-I/O offset representation.

A zero data offset must be allowed. `sr_validate_io()` currently treats a zero
`ssd_data_blkno` as invalid, so that check needs a transient exception.

## Synthetic in-memory state

Allocate the same native-shaped state expected by generic softraid code:

- one `struct sr_discipline`
- one zeroed `struct sr_metadata`
- one online `struct sr_chunk`
- one-element `sv_chunks` array and chunk list
- one open backing block vnode and device number
- online volume and chunk status
- synthetic vendor, product, revision, UUID, and display name
- exposed size of `data_length / DEV_BSIZE`
- exposed sector size of `DEV_BSIZE`
- data block number of `data_offset / DEV_BSIZE`

Set `sd_meta_type` to the native type so existing free paths can handle the
synthetic allocation, but add an explicit transient flag to distinguish it
from real native metadata.

Do not create or link crypto optional-metadata objects. Transient key and IV
state is runtime state in `struct sr_crypto`, not metadata.

The transient create path should disable callbacks that are irrelevant or
unsafe for this mode:

- no create or assemble callback is used
- no discipline-specific passphrase ioctl
- no discard callback in the first version
- no auto-assemble capability

## Minimal CRYPTO changes

Add transient state to the existing runtime CRYPTO structure, for example:

```c
#define SR_CRYPTOF_TRANSIENT	0x01

uint32_t	scr_flags;
uint64_t	scr_iv_offset;
```

The supplied key is stored in the existing `scr_key[0]`, and the provider
session is stored in the existing `scr_sid[0]`.

### Resource allocation

In `sr_crypto_alloc_resources_internal()`, retain the existing native path
unchanged. For a transient discipline:

1. set `scr_alg` to `CRYPTO_AES_XTS`
2. set `scr_klen` to 512 bits
3. allocate the existing WUs, CCBs, crypto requests, and bounce buffers
4. skip `sr_crypto_decrypt_key()`
5. create only `scr_sid[0]` from `scr_key[0]`

The existing resource-free path can be reused without a transient variant.

### Descriptor preparation

Retain the existing one-descriptor-per-`DEV_BSIZE` layout. In
`sr_crypto_prepare()`, transient mode makes only two substitutions:

- always select `scr_sid[0]`
- use `scr_iv_offset + blkno` as the descriptor block number

Backing I/O still uses the unmodified virtual block number. The existing
`ssd_data_blkno` addition in `sr_ccb_rw()` translates it to the backing
payload:

```text
backing_block = ssd_data_blkno + virtual_block
tweak_block   = scr_iv_offset + virtual_block
```

On the initial little-endian targets, copying the resulting 64-bit block
number into `crd_iv` preserves native CRYPTO behavior and implements
`plain64`.

No other changes should be made to write encryption, lower submission, read
completion, decryption, provider migration handling, residual handling, or
error completion as part of this feature.

## Metadata suppression

The dedicated create path must not call:

- `sr_meta_probe()`
- `sr_meta_attach()`
- `sr_meta_read()`
- `sr_meta_clear()`
- `sr_meta_init()`
- `sr_meta_save()`
- `sr_roam_chunks()`

Generic deletion and system shutdown currently request a metadata save.
Change those call sites, or `sr_discipline_shutdown()`, to skip
`sr_meta_save()` when the discipline is transient.

This is the only metadata special case required for the initial path.
Synthetic metadata exists solely to satisfy generic runtime consumers.

## Backing-device assumptions

Use the same practical ownership model as existing softraid rather than
adding a new disk-layer claim mechanism.

The create path should:

- reject a device already used by softraid
- open the block vnode for read and write
- rely on the block-vnode open to reject a mounted partition
- retain the vnode until normal discipline shutdown

The user must not access the backing block or raw character device while the
mapping exists. Strong prevention of concurrent raw or buffered access is out
of scope for the first version.

## Creation sequence

The dedicated transient create ioctl should:

1. copy the configuration and arrange to clear all temporary key storage
2. validate the fixed 512-byte geometry and backing range
3. reject a backing device already used by softraid
4. open the backing block vnode for read and write
5. allocate the discipline and one-thread taskq
6. call `sr_crypto_discipline_init()`
7. mark the discipline and CRYPTO runtime state transient
8. remove auto-assemble and unsupported callbacks
9. allocate and populate synthetic metadata and one online chunk
10. copy the key to `scr_key[0]` and store `scr_iv_offset`
11. insert the discipline in the normal softraid discipline list
12. call the existing CRYPTO resource allocator
13. initialize volume state and the SCSI I/O pool
14. attach the discipline to a free softraid SCSI target
15. mark the discipline ready
16. clear the ioctl and temporary key copies

The attachment portion can be a small, trimmed copy of the latter part of
`sr_ioctl_createraid()`. Avoid refactoring the native create path unless doing
so is clearly smaller and does not change its behavior.

Failure unwind should use the existing discipline shutdown/free machinery
where possible. No failure path may read, clear, or write backing metadata.

## Deletion and shutdown

Use the existing `BIOCDELETERAID` and generic discipline shutdown behavior.
The only transient-specific rule is to skip metadata saving.

Existing shutdown should continue to:

1. stop admission and detach the SCSI LUN
2. destroy the discipline taskq
3. free CRYPTO resources and the provider session
4. close and release the backing vnode
5. clear and free the discipline and synthetic state

Do not add new draining, flushing, force-delete, or error-recovery semantics
for the initial implementation. The transient mapping should behave like
native softraid CRYPTO except that it has no on-disk softraid metadata and
uses one externally supplied key and IV offset.

## Expected implementation size

Most new code is the dedicated ioctl, geometry validation, synthetic chunk
construction, and trimmed SCSI attachment sequence. The hot CRYPTO path should
need only a few transient conditionals.

A reasonable initial estimate is:

- approximately 250-400 lines of new kernel code
- fewer than 30 changed lines in the CRYPTO resource and I/O path
- a small userland ioctl wrapper, separate from container parsing and key
  derivation

This approach maximizes reuse and keeps runtime performance effectively
identical to existing softraid CRYPTO.
