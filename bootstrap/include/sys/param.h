/*
 * Shim <sys/param.h> - add BSD constants missing on glibc.
 */
#include_next <sys/param.h>

#ifndef BOOTSTRAP_SYS_PARAM_H
#define BOOTSTRAP_SYS_PARAM_H

#ifndef MAXBSIZE
#define MAXBSIZE (64 * 1024)
#endif

#endif /* BOOTSTRAP_SYS_PARAM_H */
