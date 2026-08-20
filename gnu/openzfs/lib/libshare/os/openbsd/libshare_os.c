// SPDX-License-Identifier: CDDL-1.0

#include <libshare.h>

#include "libshare_impl.h"

static int
unsupported_share(sa_share_impl_t share)
{
	(void)share;
	return (SA_NOT_SUPPORTED);
}

static int
unsupported_shareopts(const char *shareopts)
{
	(void)shareopts;
	return (SA_NOT_SUPPORTED);
}

static boolean_t
unsupported_is_shared(sa_share_impl_t share)
{
	(void)share;
	return (B_FALSE);
}

static int
unsupported_commit(void)
{
	return (SA_OK);
}

const sa_fstype_t libshare_nfs_type = {
	.enable_share = unsupported_share,
	.disable_share = unsupported_share,
	.is_shared = unsupported_is_shared,
	.validate_shareopts = unsupported_shareopts,
	.commit_shares = unsupported_commit,
};

const sa_fstype_t libshare_smb_type = {
	.enable_share = unsupported_share,
	.disable_share = unsupported_share,
	.is_shared = unsupported_is_shared,
	.validate_shareopts = unsupported_shareopts,
	.commit_shares = unsupported_commit,
};
