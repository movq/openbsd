// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 * You can obtain a copy of the License at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 *
 * CDDL HEADER END
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/vdev.h>

/*
 * Check whether the reserved boot area is in use.
 *
 * OpenBSD boot loaders do not currently use the ZFS boot-reserved area.
 * RAID-Z expansion may therefore use it as scratch space.
 */
int
vdev_check_boot_reserve(spa_t *spa, vdev_t *childvd)
{
	(void) spa;
	(void) childvd;

	return (0);
}
