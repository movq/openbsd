/*	$OpenBSD: walk.c,v 1.12 2023/08/19 04:21:06 guenther Exp $	*/
/*	$NetBSD: walk.c,v 1.29 2015/11/25 00:48:49 christos Exp $	*/

/*
 * Copyright (c) 2001 Wasabi Systems, Inc.
 * All rights reserved.
 *
 * Written by Luke Mewburn for Wasabi Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed for the NetBSD Project by
 *      Wasabi Systems, Inc.
 * 4. The name of Wasabi Systems, Inc. may not be used to endorse
 *    or promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY WASABI SYSTEMS, INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL WASABI SYSTEMS, INC
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "makefs.h"

static	fsnode	*create_fsnode(const char *, const char *, const char *,
			       struct stat *);
static	fsnode	*create_synthetic_fsnode(const char *, const char *,
			       const char *, mode_t, uid_t, gid_t, dev_t);
static	fsnode	*find_child(fsnode *, const char *);
static	fsnode	*ensure_dir(fsnode *, const char *);
static	void	install_devnode(fsnode *, const char *, mode_t, uid_t, gid_t,
			       dev_t);
static	fsinode	*link_check(fsinode *);
static	uint64_t parse_unsigned(const char *, const char *, unsigned long,
			       uint64_t);
static	dev_t	openbsd_makedev(unsigned int, unsigned int);


/*
 * walk_dir --
 *	build a tree of fsnodes from `root' and `dir', with a parent
 *	fsnode of `parent' (which may be NULL for the root of the tree).
 *	append the tree to a fsnode of `join' if it is not NULL.
 *	each "level" is a directory, with the "." entry guaranteed to be
 *	at the start of the list, and without ".." entries.
 */
fsnode *
walk_dir(const char *root, const char *dir, fsnode *parent, fsnode *join)
{
	fsnode		*first, *cur, *prev, *last;
	DIR		*dirp;
	struct dirent	*dent;
	char		path[PATH_MAX+1];
	struct stat	stbuf;
	char		*name, *rp;
	int		dot, len;

	assert(root != NULL);
	assert(dir != NULL);

	len = snprintf(path, sizeof(path), "%s/%s", root, dir);
	if (len >= (int)sizeof(path))
		errx(1, "Pathname too long.");
	if ((dirp = opendir(path)) == NULL)
		err(1, "Can't opendir `%s'", path);
	rp = path + strlen(root) + 1;
	if (join != NULL) {
		first = cur = join;
		while (cur->next != NULL)
			cur = cur->next;
		prev = last = cur;
	} else
		last = first = prev = NULL;
	while ((dent = readdir(dirp)) != NULL) {
		name = dent->d_name;
		dot = 0;
		if (name[0] == '.')
			switch (name[1]) {
			case '\0':	/* "." */
				if (join != NULL)
					continue;
				dot = 1;
				break;
			case '.':	/* ".." */
				if (name[2] == '\0')
					continue;
				/* FALLTHROUGH */
			default:
				dot = 0;
			}
		if (snprintf(path + len, sizeof(path) - len, "/%s", name) >=
		    (int)sizeof(path) - len)
			errx(1, "Pathname too long.");
		if (lstat(path, &stbuf) == -1)
			err(1, "Can't lstat `%s'", path);
		if (S_ISSOCK(stbuf.st_mode & S_IFMT))
			continue;

		if (join != NULL) {
			cur = join->next;
			for (;;) {
				if (cur == NULL || strcmp(cur->name, name) == 0)
					break;
				if (cur == last) {
					cur = NULL;
					break;
				}
				cur = cur->next;
			}
			if (cur != NULL) {
				if (S_ISDIR(cur->type) &&
				    S_ISDIR(stbuf.st_mode)) {
					cur->child = walk_dir(root, rp, cur,
					    cur->child);
					continue;
				}
				errx(1, "Can't merge %s `%s' with "
				    "existing %s",
				    inode_type(stbuf.st_mode), path,
				    inode_type(cur->type));
			}
		}

		cur = create_fsnode(root, dir, name, &stbuf);
		cur->parent = parent;
		if (dot) {
				/* ensure "." is at the start of the list */
			cur->next = first;
			first = cur;
			if (! prev)
				prev = cur;
			cur->first = first;
		} else {			/* not "." */
			if (prev)
				prev->next = cur;
			prev = cur;
			if (!first)
				first = cur;
			cur->first = first;
			if (S_ISDIR(cur->type)) {
				cur->child = walk_dir(root, rp, cur, NULL);
				continue;
			}
		}
		if (stbuf.st_nlink > 1) {
			fsinode	*curino;

			curino = link_check(cur->inode);
			if (curino != NULL) {
				free(cur->inode);
				cur->inode = curino;
				cur->inode->nlink++;
			}
		}
		if (S_ISLNK(cur->type)) {
			char	slink[PATH_MAX+1];
			int	llen;

			llen = readlink(path, slink, sizeof(slink) - 1);
			if (llen == -1)
				err(1, "Readlink `%s'", path);
			slink[llen] = '\0';
			cur->symlink = estrdup(slink);
		}
	}
	assert(first != NULL);
	if (join == NULL)
		for (cur = first->next; cur != NULL; cur = cur->next)
			cur->first = first;
	if (closedir(dirp) == -1)
		err(1, "Can't closedir `%s/%s'", root, dir);
	return (first);
}

void
apply_devspec(fsnode *root, const char *spec)
{
	FILE *fp;
	char *line = NULL, *p, *save, *path, *type, *majstr, *minstr;
	char *modestr, *uidstr, *gidstr, *extra;
	size_t linesz = 0;
	ssize_t linelen;
	unsigned int major, minor;
	mode_t mode, nodetype;
	uid_t uid;
	gid_t gid;
	dev_t rdev;
	unsigned long lineno = 0;

	if ((fp = fopen(spec, "r")) == NULL)
		err(1, "Can't open device spec `%s'", spec);

	while ((linelen = getline(&line, &linesz, fp)) != -1) {
		(void)linelen;
		lineno++;
		p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '\n' || *p == '#')
			continue;

		save = NULL;
		path = strtok_r(p, " \t\r\n", &save);
		type = strtok_r(NULL, " \t\r\n", &save);
		majstr = strtok_r(NULL, " \t\r\n", &save);
		minstr = strtok_r(NULL, " \t\r\n", &save);
		modestr = strtok_r(NULL, " \t\r\n", &save);
		uidstr = strtok_r(NULL, " \t\r\n", &save);
		gidstr = strtok_r(NULL, " \t\r\n", &save);
		extra = strtok_r(NULL, " \t\r\n", &save);
		if (path == NULL || type == NULL || majstr == NULL ||
		    minstr == NULL || modestr == NULL || uidstr == NULL ||
		    gidstr == NULL)
			errx(1, "%s:%lu: expected path type major minor mode uid gid",
			    spec, lineno);
		if (extra != NULL && extra[0] != '#')
			errx(1, "%s:%lu: too many fields", spec, lineno);

		if (strcmp(type, "c") == 0 || strcmp(type, "char") == 0)
			nodetype = S_IFCHR;
		else if (strcmp(type, "b") == 0 || strcmp(type, "block") == 0)
			nodetype = S_IFBLK;
		else
			errx(1, "%s:%lu: unknown device type `%s'",
			    spec, lineno, type);

		major = parse_unsigned(spec, majstr, lineno, 0xff);
		minor = parse_unsigned(spec, minstr, lineno, 0xffffff);
		mode = parse_unsigned(spec, modestr, lineno, 07777);
		uid = parse_unsigned(spec, uidstr, lineno, UINT_MAX);
		gid = parse_unsigned(spec, gidstr, lineno, UINT_MAX);
		rdev = openbsd_makedev(major, minor);

		install_devnode(root, path, nodetype | mode, uid, gid, rdev);
	}
	if (ferror(fp))
		err(1, "Can't read device spec `%s'", spec);
	free(line);
	if (fclose(fp) == EOF)
		err(1, "Can't close device spec `%s'", spec);
}

static fsnode *
create_fsnode(const char *root, const char *path, const char *name,
    struct stat *stbuf)
{
	fsnode *cur;

	cur = ecalloc(1, sizeof(*cur));
	cur->path = estrdup(path);
	cur->name = estrdup(name);
	cur->inode = ecalloc(1, sizeof(*cur->inode));
	cur->root = root;
	cur->type = stbuf->st_mode & S_IFMT;
	cur->inode->nlink = 1;
	cur->inode->st = *stbuf;
	if (Tflag) {
		cur->inode->st.st_atim.tv_sec = stampts;
		cur->inode->st.st_atim.tv_nsec = 0;
		cur->inode->st.st_mtim = cur->inode->st.st_ctim =
		    cur->inode->st.st_atim;
	}
	return (cur);
}

static fsnode *
create_synthetic_fsnode(const char *root, const char *path, const char *name,
    mode_t mode, uid_t uid, gid_t gid, dev_t rdev)
{
	fsnode *cur;
	struct timespec ts;

	cur = ecalloc(1, sizeof(*cur));
	cur->path = estrdup(path);
	cur->name = estrdup(name);
	cur->inode = ecalloc(1, sizeof(*cur->inode));
	cur->root = root;
	cur->type = mode & S_IFMT;
	cur->inode->nlink = 1;
	cur->inode->st.st_mode = mode;
	cur->inode->st.st_uid = uid;
	cur->inode->st.st_gid = gid;
	cur->inode->st.st_rdev = rdev;
	cur->inode->st.st_size = 0;
	ts.tv_sec = Tflag ? stampts : start_time.tv_sec;
	ts.tv_nsec = Tflag ? 0 : start_time.tv_nsec;
	cur->inode->st.st_atim = ts;
	cur->inode->st.st_mtim = ts;
	cur->inode->st.st_ctim = ts;
	return (cur);
}

static fsnode *
find_child(fsnode *dir, const char *name)
{
	fsnode *cur;

	for (cur = dir; cur != NULL; cur = cur->next) {
		if (strcmp(cur->name, name) == 0)
			return (cur);
	}
	return (NULL);
}

static fsnode *
ensure_dir(fsnode *parentdot, const char *name)
{
	fsnode *dir, *dot, *last;
	char path[PATH_MAX + 1];

	dir = find_child(parentdot, name);
	if (dir != NULL) {
		if (!S_ISDIR(dir->type))
			errx(1, "Device spec path component `%s' is not a directory",
			    name);
		if (dir->child == NULL)
			errx(1, "Directory `%s' has no child list", name);
		return (dir);
	}

	dir = create_synthetic_fsnode(parentdot->root, parentdot->path, name,
	    S_IFDIR | 0755, 0, 0, 0);
	dir->parent = parentdot->parent;
	dir->first = parentdot;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", parentdot->path,
	    name) >= sizeof(path))
		errx(1, "Pathname too long.");
	dot = create_synthetic_fsnode(parentdot->root, path, ".",
	    S_IFDIR | 0755, 0, 0, 0);
	dot->parent = dir;
	dot->first = dot;
	dir->child = dot;

	for (last = parentdot; last->next != NULL; last = last->next)
		continue;
	last->next = dir;
	return (dir);
}

static void
install_devnode(fsnode *root, const char *path, mode_t mode, uid_t uid,
    gid_t gid, dev_t rdev)
{
	fsnode *parentdot, *parentdir, *old, *last, *node;
	char *copy, *p, *component, *next;

	if (path[0] == '/')
		path++;
	if (*path == '\0')
		errx(1, "Device spec path is empty");

	copy = estrdup(path);
	parentdot = root;
	parentdir = root->parent;
	p = copy;
	for (;;) {
		component = strsep(&p, "/");
		if (component == NULL || *component == '\0' ||
		    strcmp(component, ".") == 0 ||
		    strcmp(component, "..") == 0)
			errx(1, "Invalid device spec path `%s'", path);
		next = p;
		if (next == NULL)
			break;
		parentdir = ensure_dir(parentdot, component);
		parentdot = parentdir->child;
	}

	old = find_child(parentdot, component);
	if (old != NULL) {
		if (S_ISDIR(old->type))
			errx(1, "Device spec path `%s' names a directory", path);
		free_fsnodes(old);
	}

	node = create_synthetic_fsnode(parentdot->root, parentdot->path,
	    component, mode, uid, gid, rdev);
	node->parent = parentdir;
	node->first = parentdot;
	for (last = parentdot; last->next != NULL; last = last->next)
		continue;
	last->next = node;
	free(copy);
}

static uint64_t
parse_unsigned(const char *spec, const char *s, unsigned long lineno,
    uint64_t max)
{
	char *ep;
	uint64_t v;

	errno = 0;
	v = strtoull(s, &ep, 0);
	if (s[0] == '\0' || *ep != '\0' || errno == ERANGE || v > max)
		errx(1, "%s:%lu: invalid unsigned integer `%s'",
		    spec, lineno, s);
	return (v);
}

static dev_t
openbsd_makedev(unsigned int major, unsigned int minor)
{
	return (dev_t)((((major) & 0xff) << 8) | ((minor) & 0xff) |
	    (((minor) & 0xffff00) << 8));
}

/*
 * free_fsnodes --
 *	Removes node from tree and frees it and all of
 *   its descendants.
 */
void
free_fsnodes(fsnode *node)
{
	fsnode	*cur, *next;

	assert(node != NULL);

	/* for ".", start with actual parent node */
	if (node->first == node) {
		assert(node->name[0] == '.' && node->name[1] == '\0');
		if (node->parent) {
			assert(node->parent->child == node);
			node = node->parent;
		}
	}

	/* Find ourselves in our sibling list and unlink */
	if (node->first != node) {
		for (cur = node->first; cur->next; cur = cur->next) {
			if (cur->next == node) {
				cur->next = node->next;
				node->next = NULL;
				break;
			}
		}
	}

	for (cur = node; cur != NULL; cur = next) {
		next = cur->next;
		if (cur->child) {
			cur->child->parent = NULL;
			free_fsnodes(cur->child);
		}
		if (cur->inode->nlink-- == 1)
			free(cur->inode);
		if (cur->symlink)
			free(cur->symlink);
		free(cur->path);
		free(cur->name);
		free(cur);
	}
}


/*
 * inode_type --
 *	for a given inode type `mode', return a descriptive string.
 *	for most cases, uses inotype() from mtree/misc.c
 */
const char *
inode_type(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFBLK:
		return ("block");
	case S_IFCHR:
		return ("char");
	case S_IFDIR:
		return ("dir");
	case S_IFIFO:
		return ("fifo");
	case S_IFREG:
		return ("file");
	case S_IFLNK:
		return ("symlink");
	case S_IFSOCK:
		return ("socket");
	default:
		return ("unknown");
	}
}


/*
 * link_check --
 *	return pointer to fsinode matching `entry's st_ino & st_dev if it exists,
 *	otherwise add `entry' to table and return NULL
 */
/* This was borrowed from du.c and tweaked to keep an fsnode
 * pointer instead. -- dbj@netbsd.org
 */
static fsinode *
link_check(fsinode *entry)
{
	static struct entry {
		fsinode *data;
	} *htable;
	static int htshift;  /* log(allocated size) */
	static int htmask;   /* allocated size - 1 */
	static int htused;   /* 2*number of insertions */
	int h, h2;
	uint64_t tmp;
	/* this constant is (1<<64)/((1+sqrt(5))/2)
	 * aka (word size)/(golden ratio)
	 */
	const uint64_t HTCONST = 11400714819323198485ULL;
	const int HTBITS = 64;

	/* Never store zero in hashtable */
	assert(entry);

	/* Extend hash table if necessary, keep load under 0.5 */
	if (htused<<1 >= htmask) {
		struct entry *ohtable;

		if (!htable)
			htshift = 10;   /* starting hashtable size */
		else
			htshift++;   /* exponential hashtable growth */

		htmask  = (1 << htshift) - 1;
		htused = 0;

		ohtable = htable;
		htable = ecalloc(htmask+1, sizeof(*htable));
		/* populate newly allocated hashtable */
		if (ohtable) {
			int i;
			for (i = 0; i <= htmask>>1; i++)
				if (ohtable[i].data)
					link_check(ohtable[i].data);
			free(ohtable);
		}
	}

	/* multiplicative hashing */
	tmp = entry->st.st_dev;
	tmp <<= HTBITS>>1;
	tmp |=  entry->st.st_ino;
	tmp *= HTCONST;
	h  = tmp >> (HTBITS - htshift);
	h2 = 1 | ( tmp >> (HTBITS - (htshift<<1) - 1)); /* must be odd */

	/* open address hashtable search with double hash probing */
	while (htable[h].data) {
		if ((htable[h].data->st.st_ino == entry->st.st_ino) &&
		    (htable[h].data->st.st_dev == entry->st.st_dev)) {
			return htable[h].data;
		}
		h = (h + h2) & htmask;
	}

	/* Insert the current entry into hashtable */
	htable[h].data = entry;
	htused++;
	return NULL;
}
