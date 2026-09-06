#include "kerncompat.h"
#include <errno.h>
#include "check/qgroup-verify.h"

int
qgroup_verify(struct btrfs_fs_info *info, bool check_accounting)
{
	(void)info;
	(void)check_accounting;
	return -EOPNOTSUPP;
}

int
repair_qgroups(struct btrfs_fs_info *info, int *repaired, bool silent)
{
	(void)info;
	(void)repaired;
	(void)silent;
	return -EOPNOTSUPP;
}
