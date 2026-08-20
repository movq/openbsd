#include_next <sys/cdefs.h>

#ifndef BOOTSTRAP_SYS_CDEFS_H
#define BOOTSTRAP_SYS_CDEFS_H

#ifndef __dead
#define __dead __attribute__((__noreturn__))
#endif

#ifndef __printflike
#define __printflike(fmtarg, firstvararg) \
	__attribute__((__format__(__printf__, fmtarg, firstvararg)))
#endif

#ifndef _DONT_DEFINE_UNUSED
#ifndef __unused
#define __unused __attribute__((__unused__))
#endif
#endif

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

#endif /* BOOTSTRAP_SYS_CDEFS_H */
