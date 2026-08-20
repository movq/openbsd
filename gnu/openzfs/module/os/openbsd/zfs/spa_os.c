// SPDX-License-Identifier: CDDL-1.0

/* OpenBSD hooks for the platform-neutral SPA lifecycle. */

#include <sys/spa.h>

const char *
spa_history_zone(void)
{
	return ("openbsd");
}

void
spa_import_os(spa_t *spa)
{
	(void)spa;
}

void
spa_export_os(spa_t *spa)
{
	(void)spa;
}

void
spa_activate_os(spa_t *spa)
{
	(void)spa;
}

void
spa_deactivate_os(spa_t *spa)
{
	(void)spa;
}
