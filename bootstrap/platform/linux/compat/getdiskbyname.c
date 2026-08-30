/*
 * getdiskbyname(3) compatibility for Linux-hosted makefs.
 */
struct disklabel;

struct disklabel *
getdiskbyname(const char *name)
{
	(void)name;
	return 0;
}
