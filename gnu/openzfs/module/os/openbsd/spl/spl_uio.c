// SPDX-License-Identifier: CDDL-1.0
/* OpenZFS uio operations implemented on OpenBSD's native struct uio. */

#include <sys/zfs_context.h>
#include <sys/uio_impl.h>

int
zfs_uiomove(void *buf, size_t len, zfs_uio_rw_t rw, zfs_uio_t *uio)
{

	ASSERT3U(zfs_uio_rw(uio), ==, rw);
	return (uiomove(buf, len, GET_UIO_STRUCT(uio)));
}

/*
 * Copy through a clone so the caller's uio retains its original position.
 */
int
zfs_uiocopy(void *buf, size_t len, zfs_uio_rw_t rw, zfs_uio_t *uio,
    size_t *copied)
{
	struct uio clone;
	struct iovec one;
	struct iovec *iov;
	size_t size;
	int error;

	ASSERT3U(zfs_uio_rw(uio), ==, rw);
	ASSERT3S(zfs_uio_iovcnt(uio), >, 0);

	clone = *GET_UIO_STRUCT(uio);
	if (clone.uio_iovcnt == 1) {
		one = *clone.uio_iov;
		iov = &one;
	} else {
		size = (size_t)clone.uio_iovcnt * sizeof (*iov);
		iov = kmem_alloc(size, KM_SLEEP);
		memcpy(iov, clone.uio_iov, size);
	}
	clone.uio_iov = iov;

	error = uiomove(buf, len, &clone);
	*copied = zfs_uio_resid(uio) - clone.uio_resid;
	if (iov != &one)
		kmem_free(iov, size);
	return (error);
}

/* Drop bytes from a uio without touching the referenced address space. */
void
zfs_uioskip(zfs_uio_t *uio, size_t len)
{
	struct uio *native = GET_UIO_STRUCT(uio);
	struct iovec *iov;
	size_t count;

	/* Match the illumos contract: an over-large skip changes nothing. */
	if (len > native->uio_resid)
		return;

	while (len != 0) {
		ASSERT3S(native->uio_iovcnt, >, 0);
		iov = native->uio_iov;
		if (iov->iov_len == 0) {
			native->uio_iov++;
			native->uio_iovcnt--;
			continue;
		}
		count = MIN(len, iov->iov_len);
		iov->iov_base = (char *)iov->iov_base + count;
		iov->iov_len -= count;
		native->uio_offset += count;
		native->uio_resid -= count;
		len -= count;
	}
}

boolean_t
zfs_uio_page_aligned(zfs_uio_t *uio)
{
	const struct iovec *iov = GET_UIO_STRUCT(uio)->uio_iov;
	int i;

	for (i = 0; i < zfs_uio_iovcnt(uio); i++, iov++) {
		if (((uintptr_t)iov->iov_base & PAGE_MASK) != 0 ||
		    (iov->iov_len & PAGE_MASK) != 0)
			return (B_FALSE);
	}
	return (B_TRUE);
}

/*
 * OpenBSD has no ZFS direct-I/O page-pinning implementation yet.  Returning
 * an error here keeps common code from mistaking ordinary user addresses for
 * stable pages.
 */
int
zfs_uio_get_dio_pages_alloc(zfs_uio_t *uio, zfs_uio_rw_t rw)
{

	ASSERT3U(zfs_uio_rw(uio), ==, rw);
	return (SET_ERROR(EOPNOTSUPP));
}

void
zfs_uio_free_dio_pages(zfs_uio_t *uio, zfs_uio_rw_t rw)
{

	ASSERT3U(zfs_uio_rw(uio), ==, rw);
	ASSERT3P(uio->uio_dio.pages, ==, NULL);
	ASSERT0(uio->uio_dio.npages);
	uio->uio_extflg &= ~UIO_DIRECT;
}
