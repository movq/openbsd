/*
 * getdiskbyname(3) compatibility for host makefs.
 */
struct disklabel;

struct disklabel *
getdiskbyname(const char *name)
{
	(void)name;
	return 0;
}
