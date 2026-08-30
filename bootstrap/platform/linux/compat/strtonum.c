/*
 * strtonum(3) - BSD safe string-to-long-long conversion.
 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

long long
strtonum(const char *nptr, long long minval, long long maxval,
    const char **errstrp)
{
	long long val;
	char *end;

	errno = 0;
	val = strtoll(nptr, &end, 10);

	if (nptr == end || *end != '\0') {
		*errstrp = "invalid";
		return 0;
	}
	if ((val == LLONG_MIN || val == LLONG_MAX) && errno == ERANGE) {
		*errstrp = "too large";
		return 0;
	}
	if (val < minval) {
		*errstrp = "too small";
		return 0;
	}
	if (val > maxval) {
		*errstrp = "too large";
		return 0;
	}
	*errstrp = NULL;
	return val;
}
