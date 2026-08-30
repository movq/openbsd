/*
 * reallocarray(3) - BSD array realloc with overflow checking.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

void *
reallocarray(void *ptr, size_t nmemb, size_t size)
{
	if (nmemb && size > SIZE_MAX / nmemb) {
		errno = ENOMEM;
		return NULL;
	}
	return realloc(ptr, nmemb * size);
}
