// SPDX-License-Identifier: BSD-2-Clause
/* Solaris DDI helpers implemented with OpenBSD kernel facilities. */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/limits.h>
#include <sys/sunddi.h>
#include <sys/systm.h>

static int
ddi_digit(int c)
{

	if (c >= '0' && c <= '9')
		return (c - '0');
	if (c >= 'a' && c <= 'z')
		return (c - 'a' + 10);
	if (c >= 'A' && c <= 'Z')
		return (c - 'A' + 10);
	return (-1);
}

static int
ddi_strtoull_common(const char *str, char **endptr, int base,
    unsigned long long *result, int *negative)
{
	const char *p, *start;
	unsigned long long value = 0;
	int digit, error = 0;

	if (str == NULL || result == NULL || negative == NULL ||
	    (base != 0 && (base < 2 || base > 36)))
		return (EINVAL);

	p = str;
	while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
		p++;
	*negative = 0;
	if (*p == '-' || *p == '+') {
		*negative = (*p == '-');
		p++;
	}

	if ((base == 0 || base == 16) && p[0] == '0' &&
	    (p[1] == 'x' || p[1] == 'X') &&
	    (digit = ddi_digit((unsigned char)p[2])) >= 0 && digit < 16) {
		base = 16;
		p += 2;
	} else if (base == 0) {
		base = (*p == '0') ? 8 : 10;
	}

	start = p;
	while ((digit = ddi_digit((unsigned char)*p)) >= 0 && digit < base) {
		if (value > (ULLONG_MAX - (unsigned int)digit) /
		    (unsigned int)base)
			error = ERANGE;
		else if (error == 0)
			value = value * (unsigned int)base + (unsigned int)digit;
		p++;
	}
	if (p == start) {
		if (endptr != NULL)
			*endptr = (char *)str;
		return (EINVAL);
	}
	if (endptr != NULL)
		*endptr = (char *)p;
	if (error != 0)
		return (error);

	*result = value;
	return (0);
}

int
ddi_strtol(const char *str, char **endptr, int base, long *result)
{
	unsigned long long value, limit;
	int error, negative;

	if (result == NULL)
		return (EINVAL);
	error = ddi_strtoull_common(str, endptr, base, &value, &negative);
	if (error != 0)
		return (error);
	limit = negative ? (unsigned long long)LONG_MAX + 1 : LONG_MAX;
	if (value > limit)
		return (ERANGE);
	if (negative && value == (unsigned long long)LONG_MAX + 1)
		*result = LONG_MIN;
	else
		*result = negative ? -(long)value : (long)value;
	return (0);
}

int
ddi_strtoll(const char *str, char **endptr, int base, long long *result)
{
	unsigned long long value, limit;
	int error, negative;

	if (result == NULL)
		return (EINVAL);
	error = ddi_strtoull_common(str, endptr, base, &value, &negative);
	if (error != 0)
		return (error);
	limit = negative ? (unsigned long long)LLONG_MAX + 1 : LLONG_MAX;
	if (value > limit)
		return (ERANGE);
	if (negative && value == (unsigned long long)LLONG_MAX + 1)
		*result = LLONG_MIN;
	else
		*result = negative ? -(long long)value : (long long)value;
	return (0);
}

int
ddi_strtoull(const char *str, char **endptr, int base,
    unsigned long long *result)
{
	unsigned long long value;
	int error, negative;

	if (result == NULL)
		return (EINVAL);
	error = ddi_strtoull_common(str, endptr, base, &value, &negative);
	if (error != 0)
		return (error);
	*result = negative ? 0 - value : value;
	return (0);
}

int
ddi_copyin(const void *from, void *to, size_t len, int flags)
{

	if (flags & FKIOCTL) {
		memcpy(to, from, len);
		return (0);
	}
	return (copyin(from, to, len));
}

int
ddi_copyout(const void *from, void *to, size_t len, int flags)
{

	if (flags & FKIOCTL) {
		memcpy(to, from, len);
		return (0);
	}
	return (copyout(from, to, len));
}
