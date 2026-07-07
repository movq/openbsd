/*
 * Shim <ohash.h> — ensures <stdint.h> and <stddef.h> are included
 * before the real ohash.h (which uses uint32_t, size_t, ptrdiff_t
 * without including the defining headers itself).
 */
#include <stddef.h>
#include <stdint.h>
#include_next <ohash.h>
