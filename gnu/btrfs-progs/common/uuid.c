#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <uuid/uuid.h>

void
uuid_generate(uuid_t out)
{
	arc4random_buf(out, sizeof(uuid_t));
	out[6] = (out[6] & 0x0f) | 0x40;
	out[8] = (out[8] & 0x3f) | 0x80;
}

int
uuid_is_null(const uuid_t uu)
{
	static const uuid_t nil;

	return memcmp(uu, nil, sizeof(uuid_t)) == 0;
}

static int
hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int
uuid_parse(const char *in, uuid_t uu)
{
	static const int hyphens[] = { 8, 13, 18, 23 };
	int i, n = 0, hi = -1;

	if (in == NULL || strlen(in) != 36)
		return -1;
	for (i = 0; i < 36; i++) {
		int v;

		if (n < 4 && i == hyphens[n]) {
			if (in[i] != '-')
				return -1;
			n++;
			continue;
		}
		v = hexval((unsigned char)in[i]);
		if (v < 0)
			return -1;
		if (hi < 0)
			hi = v;
		else {
			uu[(i - n) / 2] = (hi << 4) | v;
			hi = -1;
		}
	}
	return 0;
}

void
uuid_unparse(const uuid_t uu, char *out)
{
	(void)snprintf(out, 37,
	    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
	    "%02x%02x%02x%02x%02x%02x",
	    uu[0], uu[1], uu[2], uu[3], uu[4], uu[5], uu[6], uu[7],
	    uu[8], uu[9], uu[10], uu[11], uu[12], uu[13], uu[14], uu[15]);
}
