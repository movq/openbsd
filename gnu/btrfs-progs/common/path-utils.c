#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/path-utils.h"
#include "common/string-utils.h"

int
path_is_block_device(const char *path)
{
	struct stat st;

	if (stat(path, &st) == -1)
		return -errno;
	return S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);
}

int
path_is_a_mount_point(const char *path)
{
	struct statfs *mntbuf;
	int i, n;

	n = getmntinfo(&mntbuf, MNT_NOWAIT);
	if (n == 0)
		return -1;
	for (i = 0; i < n; i++)
		if (strcmp(mntbuf[i].f_mntonname, path) == 0)
			return 1;
	return 0;
}

int
path_is_reg_file(const char *path)
{
	struct stat st;

	if (stat(path, &st) == -1)
		return -errno;
	return S_ISREG(st.st_mode);
}

int
path_exists(const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0)
		return 1;
	return errno == ENOENT ? 0 : -errno;
}

static int
is_same_file(const char *a, const char *b)
{
	struct stat sa, sb;
	char ra[PATH_MAX], rb[PATH_MAX];

	if (realpath(a, ra) != NULL && realpath(b, rb) != NULL &&
	    strcmp(ra, rb) == 0)
		return 1;
	if (stat(a, &sa) == -1 || stat(b, &sb) == -1)
		return errno == ENOENT ? 0 : -errno;
	if ((S_ISBLK(sa.st_mode) || S_ISCHR(sa.st_mode)) &&
	    (S_ISBLK(sb.st_mode) || S_ISCHR(sb.st_mode)) &&
	    sa.st_rdev == sb.st_rdev)
		return 1;
	return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int
is_same_loop_file(const char *a, const char *b)
{
	return is_same_file(a, b);
}

int
path_is_reg_or_block_device(const char *path)
{
	struct stat st;

	if (stat(path, &st) == -1)
		return errno == ENOENT ? 0 : -errno;
	return S_ISREG(st.st_mode) || S_ISBLK(st.st_mode) ||
	    S_ISCHR(st.st_mode);
}

char *
path_canonicalize_dm_name(const char *name)
{
	(void)name;
	return NULL;
}

char *
path_canonicalize(const char *path)
{
	char *resolved;

	if (path == NULL || *path == '\0')
		return NULL;
	resolved = realpath(path, NULL);
	return resolved != NULL ? resolved : strdup(path);
}

int
path_is_dir(const char *path)
{
	struct stat st;

	if (stat(path, &st) == -1)
		return -errno;
	return S_ISDIR(st.st_mode);
}

int
path_is_in_dir(const char *parent, const char *path)
{
	size_t len = strlen(parent);

	if (strcmp(parent, "/") == 0)
		return path[0] == '/';
	return strncmp(parent, path, len) == 0 &&
	    (path[len] == '\0' || path[len] == '/');
}

int
arg_copy_path(char *dest, const char *src, int destlen)
{
	size_t len = strlen(src);

	if (len >= PATH_MAX || len >= (size_t)destlen)
		return -ENAMETOOLONG;
	memcpy(dest, src, len + 1);
	return 0;
}

int
path_cat_out(char *out, const char *p1, const char *p2)
{
	int ret = snprintf(out, PATH_MAX, "%s%s%s", p1,
	    *p1 != '\0' && p1[strlen(p1) - 1] == '/' ? "" : "/", p2);

	return ret < 0 || ret >= PATH_MAX ? -ENAMETOOLONG : 0;
}

int
path_cat3_out(char *out, const char *p1, const char *p2, const char *p3)
{
	char tmp[PATH_MAX];
	int ret;

	ret = path_cat_out(tmp, p1, p2);
	if (ret != 0)
		return ret;
	return path_cat_out(out, tmp, p3);
}

char *
path_basename(char *path)
{
	return basename(path);
}

char *
path_dirname(char *path)
{
	return dirname(path);
}

int
path_readlink(char *dest, const char *src)
{
	ssize_t ret;

	ret = readlink(src, dest, PATH_MAX);
	if (ret < 0)
		return -errno;
	if (ret == 0)
		return -EINVAL;
	if (ret >= PATH_MAX)
		return -ENAMETOOLONG;
	dest[ret] = '\0';
	return ret;
}
