/*
 * OpenBSD ships pre-generated LLVM feature tests.  Correct the thread-name
 * API selection for the NetBSD host after loading the upstream file.
 */
#include_next <llvm/Config/config.h>

#undef HAVE_PTHREAD_GET_NAME_NP
#undef HAVE_PTHREAD_SET_NAME_NP
#define HAVE_PTHREAD_GET_NAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0

#undef HAVE_PTHREAD_GETNAME_NP
#undef HAVE_PTHREAD_SETNAME_NP
#define HAVE_PTHREAD_GETNAME_NP 1
#define HAVE_PTHREAD_SETNAME_NP 1
