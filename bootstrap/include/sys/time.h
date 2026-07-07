/*
 * Shim <sys/time.h> — matches OpenBSD behaviour (transitive includes)
 * and adds BSD extensions missing on glibc Linux.
 *
 * On OpenBSD, <sys/time.h> includes <time.h> and ultimately pulls in
 * <stdint.h> (via <sys/select.h> → … → <sys/types.h>).
 * On glibc neither <time.h> nor <stdint.h> are guaranteed after
 * including just <sys/time.h>.
 */
#include_next <sys/time.h>
#include <stdint.h>
#include <time.h>

#ifndef BOOTSTRAP_SYS_TIME_H
#define BOOTSTRAP_SYS_TIME_H

#ifndef timespeccmp
#define timespeccmp(tsp, usp, cmp) \
	(((tsp)->tv_sec == (usp)->tv_sec) ? \
	    ((tsp)->tv_nsec cmp (usp)->tv_nsec) : \
	    ((tsp)->tv_sec cmp (usp)->tv_sec))
#endif

#endif /* BOOTSTRAP_SYS_TIME_H */
