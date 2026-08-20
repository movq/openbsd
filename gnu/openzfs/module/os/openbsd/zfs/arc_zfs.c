// SPDX-License-Identifier: CDDL-1.0
/*
 * The OpenBSD kernel already has sys/dev/pci/arc.c.  config(8) names object
 * files after the source basename, so compile the portable OpenZFS ARC through
 * a uniquely named translation unit rather than replacing the Areca driver.
 */

#include "../../../zfs/arc.c"
