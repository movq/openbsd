// SPDX-License-Identifier: CDDL-1.0

#include <zone.h>

zoneid_t
getzoneid(void)
{
	return (GLOBAL_ZONEID);
}
