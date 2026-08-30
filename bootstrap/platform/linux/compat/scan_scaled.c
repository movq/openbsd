/*
 * scan_scaled(3) compatibility for Linux host tools.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

int
scan_scaled(char *scaled, long long *result)
{
	static const char units[] = "BKMGTPE";
	long double value, factor;
	char *end, *u;

	errno = 0;
	value = strtold(scaled, &end);
	if (end == scaled || errno != 0)
		return -1;

	while (isspace((unsigned char)*end))
		end++;

	factor = 1;
	if (*end != '\0') {
		u = (char *)units;
		while (*u != '\0' &&
		    tolower((unsigned char)*u) != tolower((unsigned char)*end)) {
			u++;
			factor *= 1024;
		}
		if (*u == '\0') {
			errno = EINVAL;
			return -1;
		}
		end++;
		if (tolower((unsigned char)*end) == 'b')
			end++;
		while (isspace((unsigned char)*end))
			end++;
		if (*end != '\0') {
			errno = EINVAL;
			return -1;
		}
	}

	value *= factor;
	if (value > LLONG_MAX || value < LLONG_MIN) {
		errno = ERANGE;
		return -1;
	}

	*result = (long long)value;
	return 0;
}
