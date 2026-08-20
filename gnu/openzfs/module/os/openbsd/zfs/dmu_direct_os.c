// SPDX-License-Identifier: CDDL-1.0
/*
 * OpenBSD does not currently provide the page-pinning and page-backed ABD
 * contract required by OpenZFS direct UIO.  Keep the DMU entry points present
 * so the portable buffered path links, but reject any accidental direct-UIO
 * request instead of treating user pages as ordinary kernel buffers.
 */

#include <sys/zfs_context.h>
#include <sys/dmu.h>
#include <sys/dmu_impl.h>

int
dmu_read_uio_direct(dnode_t *dn, zfs_uio_t *uio, uint64_t size,
    dmu_flags_t flags)
{
	(void) dn;
	(void) uio;
	(void) size;
	(void) flags;

	return (SET_ERROR(EOPNOTSUPP));
}

int
dmu_write_uio_direct(dnode_t *dn, zfs_uio_t *uio, uint64_t size,
    dmu_flags_t flags, dmu_tx_t *tx)
{
	(void) dn;
	(void) uio;
	(void) size;
	(void) flags;
	(void) tx;

	return (SET_ERROR(EOPNOTSUPP));
}
