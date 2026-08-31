/*
 * FreeBSD has a different disklabel ABI.  makefs writes OpenBSD labels, so
 * use the target definitions from the source tree.
 */
#include "../../../../../sys/sys/disklabel.h"
